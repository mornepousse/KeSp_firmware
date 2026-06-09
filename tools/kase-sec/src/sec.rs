//! Module SEC : commandes de sécurité KaSe (slots secrets).
//!
//! Fournit des constructeurs de payload typés pour les commandes `SEC_*`
//! et des parsers pour leurs réponses — sans aucun couplage au transport.
//!
//! ## Commandes disponibles
//! - [`SEC_SET_SLOT`]   (`0xC0`) : provisionner un slot secret
//! - [`SEC_CLEAR_SLOT`] (`0xC1`) : effacer un slot secret
//! - [`SEC_LIST`]       (`0xC2`) : lister les slots provisionnés
//!
//! ## Contraintes du firmware
//! - 4 slots (index 0..3)
//! - Label : 16 octets null-padded (max 15 caractères visibles)
//! - Secret : max 64 octets
//! - Seul type supporté : `HmacSha1` (1) — `Empty` (0) ne peut pas être provisionné

use anyhow::{bail, Result};

// ─── Identifiants de commandes ────────────────────────────────────────────────

/// `SEC_SET_SLOT` : provisionnement d'un secret dans un slot.
pub const SEC_SET_SLOT: u8 = 0xC0;

/// `SEC_CLEAR_SLOT` : effacement d'un slot secret.
pub const SEC_CLEAR_SLOT: u8 = 0xC1;

/// `SEC_LIST` : liste des slots provisionnés (sans les secrets).
pub const SEC_LIST: u8 = 0xC2;

// ─── Type de slot ─────────────────────────────────────────────────────────────

/// Type d'un slot secret sur le dongle.
///
/// `Empty` (0) est l'état d'usine d'un slot non configuré.
/// Il ne peut **pas** être utilisé comme type lors d'un provisionnement.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum SlotType {
    /// Slot vide (non provisionné) — valeur firmware : 0
    Empty = 0,
    /// Secret HMAC-SHA1 — valeur firmware : 1
    HmacSha1 = 1,
}

impl SlotType {
    /// Convertit un octet du firmware en `SlotType`.
    ///
    /// Tout octet inconnu est traité comme `Empty` par convention défensive.
    pub fn from_u8(value: u8) -> SlotType {
        match value {
            1 => SlotType::HmacSha1,
            _ => SlotType::Empty,
        }
    }

    /// Retourne le nom lisible du type (pour l'affichage CLI).
    pub fn as_str(self) -> &'static str {
        match self {
            SlotType::Empty    => "empty",
            SlotType::HmacSha1 => "hmac-sha1",
        }
    }
}

// ─── Information de slot ──────────────────────────────────────────────────────

/// Informations d'un slot retournées par `SEC_LIST`.
///
/// Le secret n'est **jamais** inclus dans les réponses LIST — il ne sort
/// pas du dongle.
#[derive(Debug)]
pub struct SlotInfo {
    /// Index du slot sur le dongle (0..3)
    pub idx: u8,
    /// Type du secret stocké
    pub slot_type: SlotType,
    /// Label UTF-8 associé au slot (au plus 15 caractères)
    pub label: String,
}

// ─── Constantes de dimension ──────────────────────────────────────────────────

/// Taille fixe du champ label dans tous les payloads (16 octets).
const LABEL_FIELD_SIZE: usize = 16;

/// Longueur maximale du label visible (le dernier octet est réservé au NUL).
const LABEL_MAX_CHARS: usize = LABEL_FIELD_SIZE - 1; // 15

/// Longueur maximale d'un secret en octets.
const SECRET_MAX_BYTES: usize = 64;

/// Index maximal valide d'un slot.
const SLOT_MAX_IDX: u8 = 3;

/// Taille d'une entrée dans la réponse SEC_LIST :
/// idx(1) + type(1) + label(16) = 18 octets.
const LIST_ENTRY_SIZE: usize = 1 + 1 + LABEL_FIELD_SIZE;

// ─── Constructeurs de payload ─────────────────────────────────────────────────

/// Construit le payload d'une requête `SEC_SET_SLOT`.
///
/// Layout du payload :
/// ```text
/// [idx:u8][type:u8][label:16 octets null-padded][secret_len:u8][secret: secret_len octets]
/// ```
///
/// ## Erreurs
/// - `idx > 3`
/// - `slot_type == Empty` (type non provisionnable)
/// - `label.len() > 15`
/// - `secret.len() > 64`
pub fn set_slot_payload(
    idx: u8,
    slot_type: SlotType,
    label: &str,
    secret: &[u8],
) -> Result<Vec<u8>> {
    // ── Validation de l'index ─────────────────────────────────────────────
    if idx > SLOT_MAX_IDX {
        bail!(
            "Index de slot invalide : {} (valeurs valides : 0..{})",
            idx,
            SLOT_MAX_IDX
        );
    }

    // ── Validation du type (EMPTY ne peut pas être provisionné) ───────────
    if slot_type == SlotType::Empty {
        bail!("Impossible de provisionner un slot avec le type EMPTY (0)");
    }

    // ── Validation de la longueur du label ────────────────────────────────
    // On vérifie la longueur en octets UTF-8, cohérent avec le firmware
    // qui stocke des octets bruts.
    if label.len() > LABEL_MAX_CHARS {
        bail!(
            "Label trop long : {} octets (maximum {} octets)",
            label.len(),
            LABEL_MAX_CHARS
        );
    }

    // ── Validation de la longueur du secret ───────────────────────────────
    if secret.len() > SECRET_MAX_BYTES {
        bail!(
            "Secret trop long : {} octets (maximum {})",
            secret.len(),
            SECRET_MAX_BYTES
        );
    }

    // ── Construction du champ label ───────────────────────────────────────
    // 16 octets : le texte du label suivi de NUL (0x00) jusqu'à la fin.
    // Le tableau est initialisé à 0, donc label_field[15] est déjà NUL.
    let mut label_field = [0u8; LABEL_FIELD_SIZE];
    let label_bytes = label.as_bytes();
    label_field[..label_bytes.len()].copy_from_slice(label_bytes);

    // ── Assemblage du payload ─────────────────────────────────────────────
    let capacity = 1 + 1 + LABEL_FIELD_SIZE + 1 + secret.len();
    let mut payload = Vec::with_capacity(capacity);

    payload.push(idx);
    payload.push(slot_type as u8);
    payload.extend_from_slice(&label_field);
    payload.push(secret.len() as u8); // secret_len : toujours ≤ 64, tient dans u8
    payload.extend_from_slice(secret);

    Ok(payload)
}

/// Construit le payload d'une requête `SEC_CLEAR_SLOT`.
///
/// Layout du payload : `[idx:u8]`
///
/// ## Erreurs
/// - `idx > 3`
pub fn clear_slot_payload(idx: u8) -> Result<Vec<u8>> {
    if idx > SLOT_MAX_IDX {
        bail!(
            "Index de slot invalide : {} (valeurs valides : 0..{})",
            idx,
            SLOT_MAX_IDX
        );
    }
    Ok(vec![idx])
}

/// Retourne le payload (vide) d'une requête `SEC_LIST`.
///
/// La commande LIST n'a pas de paramètres.
pub fn list_payload() -> Vec<u8> {
    Vec::new()
}

// ─── Parsers de réponse ───────────────────────────────────────────────────────

/// Décode le payload d'une réponse `SEC_LIST`.
///
/// Layout du payload reçu :
/// ```text
/// [count:u8] [idx:u8][type:u8][label:16 octets] × count
/// ```
///
/// Seuls les slots non vides sont listés. Les secrets ne sont jamais inclus.
///
/// ## Erreurs
/// - Payload vide (pas même l'octet `count`)
/// - Payload trop court pour le nombre de slots annoncé
pub fn parse_list(payload: &[u8]) -> Result<Vec<SlotInfo>> {
    // ── Vérification minimale : au moins l'octet count ────────────────────
    if payload.is_empty() {
        bail!("Payload SEC_LIST vide : au moins 1 octet (count) attendu");
    }

    let slot_count = payload[0] as usize;

    // ── Vérification de la taille totale ──────────────────────────────────
    // 1 octet pour count + LIST_ENTRY_SIZE octets par slot
    let expected_len = 1 + slot_count * LIST_ENTRY_SIZE;
    if payload.len() < expected_len {
        bail!(
            "Payload SEC_LIST trop court : {} octets pour {} slots (attendu {} octets)",
            payload.len(),
            slot_count,
            expected_len
        );
    }

    // ── Décodage de chaque entrée ─────────────────────────────────────────
    let mut slots = Vec::with_capacity(slot_count);

    for i in 0..slot_count {
        // Offset de début de cette entrée dans le payload
        let entry_offset = 1 + i * LIST_ENTRY_SIZE;

        let idx       = payload[entry_offset];
        let type_byte = payload[entry_offset + 1];

        // 16 octets de label (null-padded)
        let label_start = entry_offset + 2;
        let label_end   = entry_offset + 2 + LABEL_FIELD_SIZE;
        let label_raw   = &payload[label_start..label_end];

        // On s'arrête au premier NUL pour extraire la partie visible
        let visible_end = label_raw
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(LABEL_FIELD_SIZE);
        let label = String::from_utf8_lossy(&label_raw[..visible_end]).into_owned();

        let slot_type = SlotType::from_u8(type_byte);

        slots.push(SlotInfo { idx, slot_type, label });
    }

    Ok(slots)
}

// ─── Tests unitaires ─────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    // ── set_slot_payload : layout des octets ──────────────────────────────

    #[test]
    fn set_slot_payload_layout_idx_type_label_secret() {
        let secret = b"my_secret_data";
        let result = set_slot_payload(1, SlotType::HmacSha1, "github", secret)
            .expect("payload valide attendu");

        // Octet 0 : index du slot
        assert_eq!(result[0], 1, "idx doit être 1");

        // Octet 1 : type du slot
        assert_eq!(result[1], 1, "type doit être 1 (HmacSha1)");

        // Octets 2..18 : label (16 octets, null-padded)
        let label_bytes = b"github";
        let label_field = &result[2..18];
        assert_eq!(&label_field[..label_bytes.len()], label_bytes, "texte du label incorrect");
        for i in label_bytes.len()..LABEL_FIELD_SIZE {
            assert_eq!(label_field[i], 0, "label_field[{}] doit être NUL", i);
        }

        // Octet 18 : longueur du secret
        let secret_len_offset = 2 + LABEL_FIELD_SIZE;
        assert_eq!(
            result[secret_len_offset],
            secret.len() as u8,
            "secret_len incorrect"
        );

        // Octets suivants : le secret lui-même
        let secret_start = secret_len_offset + 1;
        assert_eq!(&result[secret_start..], secret, "octets du secret incorrects");
    }

    #[test]
    fn set_slot_payload_taille_totale_coherente() {
        let secret = [0xAAu8; 20];
        let result = set_slot_payload(0, SlotType::HmacSha1, "test", &secret)
            .expect("payload valide attendu");
        // 1(idx) + 1(type) + 16(label) + 1(secret_len) + 20(secret) = 39
        assert_eq!(result.len(), 39, "taille totale incorrecte");
    }

    #[test]
    fn set_slot_payload_dernier_octet_label_est_nul() {
        // Label de 15 caractères (longueur maximale autorisée)
        // Le champ fait 16 octets → label_field[15] doit rester NUL
        let label_max = "a".repeat(LABEL_MAX_CHARS);
        let result = set_slot_payload(0, SlotType::HmacSha1, &label_max, &[0x01])
            .expect("payload valide attendu");

        // label_field commence à l'offset 2, label_field[15] = result[17]
        let last_label_byte_offset = 2 + LABEL_FIELD_SIZE - 1;
        assert_eq!(
            result[last_label_byte_offset],
            0,
            "dernier octet du champ label doit être NUL"
        );
    }

    #[test]
    fn set_slot_payload_rejette_idx_invalide() {
        let err = set_slot_payload(4, SlotType::HmacSha1, "test", &[0x01])
            .expect_err("idx=4 doit être rejeté");
        assert!(err.to_string().contains("invalide"), "message d'erreur doit mentionner 'invalide'");
    }

    #[test]
    fn set_slot_payload_rejette_type_empty() {
        let err = set_slot_payload(0, SlotType::Empty, "test", &[0x01])
            .expect_err("type EMPTY doit être rejeté");
        assert!(err.to_string().contains("EMPTY"), "message d'erreur doit mentionner 'EMPTY'");
    }

    #[test]
    fn set_slot_payload_rejette_label_trop_long() {
        let label_trop_long = "a".repeat(LABEL_FIELD_SIZE); // 16 chars > 15 max
        let err = set_slot_payload(0, SlotType::HmacSha1, &label_trop_long, &[0x01])
            .expect_err("label trop long doit être rejeté");
        assert!(err.to_string().contains("trop long"), "message doit mentionner 'trop long'");
    }

    #[test]
    fn set_slot_payload_rejette_secret_trop_long() {
        let secret_trop_long = vec![0u8; SECRET_MAX_BYTES + 1]; // 65 octets > 64 max
        let err = set_slot_payload(0, SlotType::HmacSha1, "test", &secret_trop_long)
            .expect_err("secret trop long doit être rejeté");
        assert!(err.to_string().contains("trop long"), "message doit mentionner 'trop long'");
    }

    #[test]
    fn set_slot_payload_accepte_secret_vide() {
        // Un secret vide est techniquement valide selon le protocole
        let result = set_slot_payload(0, SlotType::HmacSha1, "test", &[])
            .expect("secret vide doit être accepté");
        let secret_len_offset = 2 + LABEL_FIELD_SIZE;
        assert_eq!(result[secret_len_offset], 0, "secret_len doit être 0");
    }

    // ── clear_slot_payload ────────────────────────────────────────────────

    #[test]
    fn clear_slot_payload_contient_seulement_idx() {
        let result = clear_slot_payload(2).expect("idx=2 valide");
        assert_eq!(result, vec![2], "payload doit être [idx]");
    }

    #[test]
    fn clear_slot_payload_rejette_idx_invalide() {
        let err = clear_slot_payload(5).expect_err("idx=5 doit être rejeté");
        assert!(err.to_string().contains("invalide"));
    }

    #[test]
    fn clear_slot_payload_accepte_tous_les_indices_valides() {
        for idx in 0..=SLOT_MAX_IDX {
            let result = clear_slot_payload(idx)
                .unwrap_or_else(|_| panic!("idx={} doit être valide", idx));
            assert_eq!(result, vec![idx], "payload pour idx={} incorrect", idx);
        }
    }

    // ── parse_list ────────────────────────────────────────────────────────

    /// Construit un payload de réponse SEC_LIST synthétique.
    fn make_list_payload(slots: &[(u8, SlotType, &str)]) -> Vec<u8> {
        let mut payload = vec![slots.len() as u8];
        for (idx, slot_type, label) in slots {
            payload.push(*idx);
            payload.push(*slot_type as u8);

            let mut label_field = [0u8; LABEL_FIELD_SIZE];
            let label_bytes = label.as_bytes();
            label_field[..label_bytes.len()].copy_from_slice(label_bytes);
            payload.extend_from_slice(&label_field);
        }
        payload
    }

    #[test]
    fn parse_list_liste_vide_retourne_vecteur_vide() {
        let payload = make_list_payload(&[]);
        let slots = parse_list(&payload).expect("payload valide");
        assert!(slots.is_empty(), "aucun slot attendu");
    }

    #[test]
    fn parse_list_un_slot_decode_correctement() {
        let payload = make_list_payload(&[(0, SlotType::HmacSha1, "github")]);
        let slots = parse_list(&payload).expect("payload valide");

        assert_eq!(slots.len(), 1);
        assert_eq!(slots[0].idx, 0);
        assert_eq!(slots[0].slot_type, SlotType::HmacSha1);
        assert_eq!(slots[0].label, "github");
    }

    #[test]
    fn parse_list_plusieurs_slots_dans_lordre() {
        let entries = [
            (0u8, SlotType::HmacSha1, "totp_github"),
            (2u8, SlotType::HmacSha1, "totp_work"),
        ];
        let payload = make_list_payload(&entries);
        let slots = parse_list(&payload).expect("payload valide");

        assert_eq!(slots.len(), 2);
        assert_eq!(slots[0].idx, 0);
        assert_eq!(slots[0].label, "totp_github");
        assert_eq!(slots[1].idx, 2);
        assert_eq!(slots[1].label, "totp_work");
    }

    #[test]
    fn parse_list_label_null_padded_tronque_correctement() {
        // Le label "ab" est suivi de 14 NUL dans le champ 16-octets.
        // parse_list ne doit retourner que "ab", pas "ab\0\0\0...".
        let payload = make_list_payload(&[(0, SlotType::HmacSha1, "ab")]);
        let slots = parse_list(&payload).expect("payload valide");
        assert_eq!(slots[0].label, "ab", "les NUL de padding ne doivent pas apparaître");
    }

    #[test]
    fn parse_list_rejette_payload_vide() {
        let err = parse_list(&[]).expect_err("payload vide doit être rejeté");
        assert!(err.to_string().contains("vide"));
    }

    #[test]
    fn parse_list_rejette_payload_trop_court_pour_count() {
        // count = 1 mais aucune entrée de slot fournie
        let payload = vec![0x01u8];
        let err = parse_list(&payload).expect_err("payload trop court doit être rejeté");
        assert!(err.to_string().contains("trop court"));
    }
}
