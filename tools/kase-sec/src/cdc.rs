//! Module CDC : couche de transport du protocole binaire KaSe.
//!
//! Ce module fournit les primitives de communication avec le dongle :
//! - [`crc8`] : calcul du CRC-8 sur le payload uniquement
//! - [`build_request`] : construction d'un frame de requête KS
//! - [`parse_response`] : décodage d'un frame de réponse KR
//! - Le trait [`Transport`] : abstraction du canal de communication
//! - [`SerialTransport`] : implémentation série USB CDC réelle
//!
//! ## Format des frames
//!
//! Requête KS :  `[0x4B][0x53][cmd:u8][len:u16 LE][payload...][crc8]`
//! Réponse KR :  `[0x4B][0x52][cmd:u8][status:u8][len:u16 LE][payload...][crc8]`
//!
//! Le CRC est calculé sur le **payload uniquement** (pas sur l'en-tête).

use anyhow::{bail, Context, Result};

// ─── Bytes magiques ───────────────────────────────────────────────────────────

/// Byte magique 0 d'une requête : 'K'
const MAGIC_REQ_0: u8 = 0x4B;
/// Byte magique 1 d'une requête : 'S'
const MAGIC_REQ_1: u8 = 0x53;

/// Byte magique 0 d'une réponse : 'K'
const MAGIC_RESP_0: u8 = 0x4B;
/// Byte magique 1 d'une réponse : 'R'
const MAGIC_RESP_1: u8 = 0x52;

/// Taille de l'en-tête d'une réponse KR en octets :
/// magic(2) + cmd(1) + status(1) + len(2) = 6
const KR_HEADER_SIZE: usize = 6;

/// Taille minimale d'un frame KR complet (en-tête + crc, sans payload)
const KR_MIN_FRAME_SIZE: usize = KR_HEADER_SIZE + 1; // + 1 octet de CRC

// ─── Codes de statut ──────────────────────────────────────────────────────────

/// Codes de statut retournés par le dongle dans les frames KR.
///
/// Ces constantes reflètent exactement les valeurs définies dans le firmware.
pub mod status {
    pub const OK          : u8 = 0x00;
    pub const ERR_UNKNOWN : u8 = 0x01;
    pub const ERR_CRC     : u8 = 0x02;
    pub const ERR_INVALID : u8 = 0x03;
    pub const ERR_RANGE   : u8 = 0x04;
    pub const ERR_BUSY    : u8 = 0x05;
    pub const ERR_OVERFLOW: u8 = 0x06;

    /// Traduit un code statut en description lisible par un humain.
    pub fn describe(code: u8) -> &'static str {
        match code {
            OK           => "OK",
            ERR_UNKNOWN  => "ERR_UNKNOWN (commande inconnue)",
            ERR_CRC      => "ERR_CRC (CRC invalide)",
            ERR_INVALID  => "ERR_INVALID (paramètre invalide)",
            ERR_RANGE    => "ERR_RANGE (index hors limites)",
            ERR_BUSY     => "ERR_BUSY (dongle occupé)",
            ERR_OVERFLOW => "ERR_OVERFLOW (données trop longues)",
            _            => "ERR_inconnue",
        }
    }
}

// ─── Structure de réponse ─────────────────────────────────────────────────────

/// Réponse décodée reçue du dongle (frame KR).
#[derive(Debug)]
pub struct Response {
    /// Identifiant de la commande (miroir de la requête)
    pub cmd: u8,
    /// Code de statut (voir module [`status`])
    pub status: u8,
    /// Payload de la réponse (peut être vide)
    pub payload: Vec<u8>,
}

// ─── Fonctions pures (testables sans matériel) ────────────────────────────────

/// Calcule le CRC-8 sur les octets du payload.
///
/// ## Algorithme
/// - Polynôme : 0x31
/// - Init     : 0x00
/// - MSB-first
/// - Pas de réflexion, pas de XOR final
///
/// Ce n'est PAS le CRC-8/MAXIM catalogué. Seul le **payload** est passé ici,
/// jamais l'en-tête.
///
/// ## Vecteurs de test connus
/// - `[]`          → `0x00`
/// - `[0x01]`      → `0x31`
/// - `[0x4B, 0x53]` → `0xBE`
pub fn crc8(payload: &[u8]) -> u8 {
    let mut crc: u8 = 0x00;

    for &byte in payload {
        // XOR du byte courant dans le registre CRC
        crc ^= byte;

        // 8 tours de traitement MSB-first, polynôme 0x31
        for _ in 0..8 {
            let msb_est_un = (crc & 0x80) != 0;

            // Décalage vers la gauche (les bits qui débordent sont perdus)
            crc = crc.wrapping_shl(1);

            // XOR avec le polynôme uniquement si le MSB était 1
            if msb_est_un {
                crc ^= 0x31;
            }
        }
    }

    crc
}

/// Construit un frame de requête KS complet prêt à envoyer.
///
/// Format : `[0x4B][0x53][cmd][len_lo][len_hi][payload...][crc8]`
///
/// Le CRC est calculé sur le payload uniquement.
pub fn build_request(cmd: u8, payload: &[u8]) -> Vec<u8> {
    // Longueur du payload encodée en u16 little-endian
    let payload_len_u16 = payload.len() as u16;
    let len_bytes = payload_len_u16.to_le_bytes();

    // CRC calculé sur le payload uniquement (pas l'en-tête)
    let crc = crc8(payload);

    // Pré-allocation : magic(2) + cmd(1) + len(2) + payload + crc(1)
    let frame_capacity = 2 + 1 + 2 + payload.len() + 1;
    let mut frame = Vec::with_capacity(frame_capacity);

    // ── En-tête ──
    frame.push(MAGIC_REQ_0); // 'K'
    frame.push(MAGIC_REQ_1); // 'S'
    frame.push(cmd);
    frame.push(len_bytes[0]); // octet de poids faible
    frame.push(len_bytes[1]); // octet de poids fort

    // ── Payload puis CRC ──
    frame.extend_from_slice(payload);
    frame.push(crc);

    frame
}

/// Décode un frame de réponse KR reçu du dongle.
///
/// Vérifie les bytes magiques et le CRC avant de retourner la [`Response`].
///
/// ## Erreurs
/// - Frame trop courte
/// - Bytes magiques KR incorrects
/// - CRC incorrect (données corrompues)
/// - Frame incomplète par rapport à la longueur annoncée
pub fn parse_response(bytes: &[u8]) -> Result<Response> {
    // ── Validation de la taille minimale ──────────────────────────────────
    if bytes.len() < KR_MIN_FRAME_SIZE {
        bail!(
            "Frame KR trop courte : {} octets (minimum attendu : {})",
            bytes.len(),
            KR_MIN_FRAME_SIZE
        );
    }

    // ── Vérification des bytes magiques 'K' 'R' ───────────────────────────
    if bytes[0] != MAGIC_RESP_0 || bytes[1] != MAGIC_RESP_1 {
        bail!(
            "Bytes magiques KR incorrects : attendu 4B 52, reçu {:02X} {:02X}",
            bytes[0],
            bytes[1]
        );
    }

    // ── Extraction des champs de l'en-tête ────────────────────────────────
    let cmd            = bytes[2];
    let response_status = bytes[3];

    // Longueur du payload en u16 little-endian (bytes 4 et 5)
    let payload_len = u16::from_le_bytes([bytes[4], bytes[5]]) as usize;

    // ── Vérification de la taille totale ──────────────────────────────────
    // En-tête(6) + payload(payload_len) + CRC(1)
    let expected_total = KR_HEADER_SIZE + payload_len + 1;
    if bytes.len() < expected_total {
        bail!(
            "Frame KR incomplète : {} octets reçus, {} attendus (payload_len={})",
            bytes.len(),
            expected_total,
            payload_len
        );
    }

    // ── Extraction du payload ─────────────────────────────────────────────
    let payload_start = KR_HEADER_SIZE;
    let payload_end   = KR_HEADER_SIZE + payload_len;
    let payload       = bytes[payload_start..payload_end].to_vec();

    // ── Vérification du CRC (calculé sur le payload uniquement) ──────────
    let received_crc = bytes[payload_end];
    let computed_crc = crc8(&payload);
    if received_crc != computed_crc {
        bail!(
            "CRC invalide : reçu {:02X}, calculé {:02X} — données corrompues ?",
            received_crc,
            computed_crc
        );
    }

    Ok(Response {
        cmd,
        status: response_status,
        payload,
    })
}

// ─── Trait Transport ──────────────────────────────────────────────────────────

/// Abstraction du canal de communication entre l'outil et le dongle.
///
/// L'envoi d'une requête et la réception de la réponse sont atomiques :
/// `transact` envoie le frame KS et retourne les octets bruts du frame KR.
///
/// Cette abstraction permet d'utiliser un mock dans les tests sans matériel.
pub trait Transport {
    /// Envoie un frame de requête KS et retourne le frame de réponse KR brut.
    fn transact(&mut self, request: &[u8]) -> Result<Vec<u8>>;
}

// ─── Transport série réel ─────────────────────────────────────────────────────

/// Transport USB CDC vers le dongle KaSe via un port série.
///
/// Le baud rate (115200) est requis par l'API `serialport` mais ignoré par
/// le firmware CDC qui fonctionne à la vitesse USB native.
pub struct SerialTransport {
    port: Box<dyn serialport::SerialPort>,
}

impl SerialTransport {
    /// Ouvre le port série au chemin indiqué.
    ///
    /// ## Paramètres
    /// - `path` : chemin du port (ex: `/dev/ttyACM1` sur Linux)
    ///
    /// ## Erreurs
    /// Retourne une erreur si le port n'existe pas ou n'est pas accessible.
    pub fn open(path: &str) -> Result<Self> {
        let timeout = std::time::Duration::from_secs(1);

        let port = serialport::new(path, 115_200)
            .timeout(timeout)
            .open()
            .with_context(|| format!("Impossible d'ouvrir le port série '{}'", path))?;

        Ok(SerialTransport { port })
    }
}

impl Transport for SerialTransport {
    fn transact(&mut self, request: &[u8]) -> Result<Vec<u8>> {
        use std::io::{Read, Write};

        // ── Envoi de la requête ───────────────────────────────────────────
        // write_all garantit que tous les octets sont envoyés, même si le
        // système d'exploitation en retient une partie en buffer.
        self.port
            .write_all(request)
            .context("Échec d'écriture de la requête CDC vers le dongle")?;

        // ── Lecture de l'en-tête KR (6 octets) ───────────────────────────
        // Format : magic(2) + cmd(1) + status(1) + len(2)
        let mut header = [0u8; KR_HEADER_SIZE];
        self.port
            .read_exact(&mut header)
            .context("Échec de lecture de l'en-tête KR (6 octets)")?;

        // ── Décodage de la longueur du payload ────────────────────────────
        let payload_len = u16::from_le_bytes([header[4], header[5]]) as usize;

        // ── Lecture du payload + CRC ──────────────────────────────────────
        // Le CRC (1 octet) suit immédiatement le payload.
        let rest_len = payload_len + 1;
        let mut rest = vec![0u8; rest_len];
        self.port
            .read_exact(&mut rest)
            .context("Échec de lecture du payload et du CRC")?;

        // ── Assemblage du frame KR complet ────────────────────────────────
        let mut frame = Vec::with_capacity(KR_HEADER_SIZE + rest_len);
        frame.extend_from_slice(&header);
        frame.extend_from_slice(&rest);

        Ok(frame)
    }
}

// ─── Tests unitaires ─────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    // ── Vecteurs de référence CRC-8 ───────────────────────────────────────
    // Ces vecteurs sont définis par le spec du protocole firmware.

    #[test]
    fn crc8_payload_vide_retourne_zero() {
        let result = crc8(&[]);
        assert_eq!(result, 0x00, "payload vide → CRC doit être 0x00");
    }

    #[test]
    fn crc8_un_octet_0x01_retourne_0x31() {
        let result = crc8(&[0x01]);
        assert_eq!(result, 0x31, "[0x01] → CRC doit être 0x31");
    }

    #[test]
    fn crc8_magic_ks_retourne_0xbe() {
        let result = crc8(&[0x4B, 0x53]);
        assert_eq!(result, 0xBE, "[0x4B, 0x53] → CRC doit être 0xBE");
    }

    // ── build_request : structure du frame ────────────────────────────────

    #[test]
    fn build_request_bytes_magiques_sont_ks() {
        let frame = build_request(0xC2, &[]);
        assert_eq!(frame[0], 0x4B, "magic[0] doit être 0x4B ('K')");
        assert_eq!(frame[1], 0x53, "magic[1] doit être 0x53 ('S')");
    }

    #[test]
    fn build_request_cmd_est_a_index_2() {
        let frame = build_request(0xC0, &[]);
        assert_eq!(frame[2], 0xC0, "cmd doit être à l'index 2");
    }

    #[test]
    fn build_request_longueur_encodee_little_endian() {
        // Payload de 3 octets → len = 0x0003 → [0x03, 0x00] en LE
        let payload = [0x01u8, 0x02, 0x03];
        let frame = build_request(0xC0, &payload);

        assert_eq!(frame[3], 0x03, "len_lo doit être 0x03 pour un payload de 3 octets");
        assert_eq!(frame[4], 0x00, "len_hi doit être 0x00 pour un payload de 3 octets");
    }

    #[test]
    fn build_request_longueur_sur_deux_octets() {
        // Payload de 256 octets → len = 0x0100 → [0x00, 0x01] en LE
        let payload = vec![0xAAu8; 256];
        let frame = build_request(0xC0, &payload);

        assert_eq!(frame[3], 0x00, "len_lo doit être 0x00 pour 256 octets");
        assert_eq!(frame[4], 0x01, "len_hi doit être 0x01 pour 256 octets");
    }

    #[test]
    fn build_request_crc_est_en_dernier_octet() {
        let payload = [0x01u8, 0x02];
        let expected_crc = crc8(&payload);
        let frame = build_request(0xC0, &payload);
        let last_byte = *frame.last().expect("frame ne doit pas être vide");

        assert_eq!(last_byte, expected_crc, "dernier octet doit être le CRC du payload");
    }

    #[test]
    fn build_request_taille_correcte_avec_payload() {
        let payload = [0x01u8, 0x02, 0x03];
        let frame = build_request(0xC0, &payload);
        // magic(2) + cmd(1) + len(2) + payload(3) + crc(1) = 9
        assert_eq!(frame.len(), 9, "taille totale incorrecte");
    }

    #[test]
    fn build_request_taille_correcte_sans_payload() {
        let frame = build_request(0xC2, &[]);
        // magic(2) + cmd(1) + len(2) + payload(0) + crc(1) = 6
        assert_eq!(frame.len(), 6, "taille totale sans payload incorrecte");
    }

    // ── parse_response : cas valides ──────────────────────────────────────

    /// Construit un frame KR valide pour les tests.
    fn make_kr_frame(cmd: u8, status: u8, payload: &[u8]) -> Vec<u8> {
        let payload_len = payload.len() as u16;
        let len_bytes = payload_len.to_le_bytes();
        let crc = crc8(payload);

        let mut frame = vec![
            0x4B, 0x52,        // magic KR
            cmd,               // commande
            status,            // code statut
            len_bytes[0],      // len_lo
            len_bytes[1],      // len_hi
        ];
        frame.extend_from_slice(payload);
        frame.push(crc);
        frame
    }

    #[test]
    fn parse_response_payload_vide_ok() {
        let frame = make_kr_frame(0xC2, 0x00, &[]);
        let response = parse_response(&frame).expect("frame valide doit être parsé");

        assert_eq!(response.cmd, 0xC2);
        assert_eq!(response.status, 0x00);
        assert!(response.payload.is_empty(), "payload doit être vide");
    }

    #[test]
    fn parse_response_avec_payload() {
        let payload = vec![0x01u8, 0x02, 0x03];
        let frame = make_kr_frame(0xC2, 0x00, &payload);
        let response = parse_response(&frame).expect("frame valide doit être parsé");

        assert_eq!(response.payload, payload, "payload doit correspondre");
    }

    #[test]
    fn parse_response_status_erreur_preserve() {
        // Le parser ne rejette pas les statuts d'erreur — c'est au code
        // appelant de décider quoi faire avec le statut.
        let frame = make_kr_frame(0xC0, 0x04, &[]); // ERR_RANGE
        let response = parse_response(&frame).expect("frame avec erreur doit être parsé");

        assert_eq!(response.status, 0x04);
    }

    // ── parse_response : cas d'erreur ─────────────────────────────────────

    #[test]
    fn parse_response_rejette_magic_invalide() {
        let frame = [
            0x00, 0x00, // magic incorrect (pas KR)
            0xC2, 0x00, 0x00, 0x00, 0x00,
        ];
        let result = parse_response(&frame);
        assert!(result.is_err(), "magic invalide doit être rejeté");
    }

    #[test]
    fn parse_response_rejette_magic_ks_comme_kr() {
        // Un frame de requête KS ne peut pas être parsé comme réponse KR
        let request_frame = build_request(0xC2, &[]);
        let result = parse_response(&request_frame);
        assert!(result.is_err(), "frame KS ne doit pas être accepté comme KR");
    }

    #[test]
    fn parse_response_rejette_crc_invalide() {
        // Payload d'un octet avec un CRC délibérément incorrect
        // crc8([0xAA]) = 0x27, donc 0xFF est incorrect
        let payload = vec![0xAAu8];
        let correct_crc = crc8(&payload);
        assert_ne!(correct_crc, 0xFF, "pré-condition : 0xFF ne doit pas être le bon CRC");

        let frame = vec![
            0x4B, 0x52, // magic KR
            0xC2,       // cmd
            0x00,       // status OK
            0x01, 0x00, // len = 1
            0xAA,       // payload (1 octet)
            0xFF,       // CRC délibérément faux
        ];

        let result = parse_response(&frame);
        assert!(result.is_err(), "CRC invalide doit être rejeté");
    }

    #[test]
    fn parse_response_rejette_frame_trop_courte() {
        // Seulement 3 octets — trop court pour un frame KR minimal
        let frame = [0x4B, 0x52, 0xC2];
        let result = parse_response(&frame);
        assert!(result.is_err(), "frame trop courte doit être rejetée");
    }

    #[test]
    fn parse_response_rejette_frame_tronquee() {
        // En-tête annonce 5 octets de payload mais le buffer est coupé
        let frame = vec![
            0x4B, 0x52, // magic KR
            0xC2, 0x00, // cmd, status
            0x05, 0x00, // len = 5
            0xAA, 0xBB, // seulement 2 octets de payload au lieu de 5 + crc
        ];
        let result = parse_response(&frame);
        assert!(result.is_err(), "frame tronquée doit être rejetée");
    }
}
