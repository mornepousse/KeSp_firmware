//! `kase-sec` — CLI de provisionnement des slots secrets du dongle KaSe.
//!
//! Communique avec le dongle via USB CDC (protocole binaire KS/KR).
//!
//! ## Utilisation
//! ```
//! kase-sec --port /dev/ttyACM1 list
//! kase-sec --port /dev/ttyACM1 provision --slot 0 --label github --secret 3132...
//! kase-sec --port /dev/ttyACM1 clear --slot 0
//! ```

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand, ValueEnum};

mod cdc;
mod sec;

use cdc::{build_request, parse_response, status, SerialTransport, Transport};
use sec::SlotType;

// ─── Définition de la CLI (clap derive) ──────────────────────────────────────

/// Format d'encodage du secret pour la commande `provision`.
#[derive(Debug, Clone, Copy, ValueEnum)]
enum SecretFormat {
    /// Hexadécimal standard (ex: `3132333435`)
    Hex,
    /// Base32 RFC4648 sans padding — format courant pour les seeds TOTP/HOTP
    /// (ex: `GEZDGNBVGY3TQOJQ`)
    Base32,
}

/// kase-sec — Outil de provisionnement des slots secrets du dongle KaSe.
///
/// Parle au dongle via USB CDC en utilisant le protocole binaire KS/KR.
/// Implémente les commandes SEC_SET_SLOT (0xC0), SEC_CLEAR_SLOT (0xC1),
/// et SEC_LIST (0xC2) du Plan 1 de sécurité.
#[derive(Parser)]
#[command(name = "kase-sec", version, about, long_about = None)]
struct Cli {
    /// Port série USB CDC du dongle (ex: /dev/ttyACM1)
    ///
    /// Requis pour toutes les commandes qui communiquent avec le dongle.
    #[arg(long, global = true)]
    port: Option<String>,

    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Liste les slots secrets provisionnés sur le dongle.
    ///
    /// Affiche l'index, le type et le label de chaque slot.
    /// Les secrets ne sont jamais retournés par le dongle.
    List,

    /// Provisionne un slot secret avec un label et une clé secrète.
    ///
    /// Exemple :
    ///   kase-sec --port /dev/ttyACM1 provision --slot 0 --label github --secret 3132...
    Provision {
        /// Index du slot à configurer (0 à 3)
        #[arg(long)]
        slot: u8,

        /// Label du slot (max 15 caractères)
        #[arg(long)]
        label: String,

        /// Valeur du secret, encodée selon --format (défaut : hex)
        #[arg(long)]
        secret: String,

        /// Format d'encodage de la valeur --secret
        #[arg(long, default_value = "hex")]
        format: SecretFormat,

        /// Type de slot (seul 'hmac-sha1' est supporté pour l'instant)
        #[arg(long = "type", default_value = "hmac-sha1")]
        slot_type: String,
    },

    /// Efface un slot secret (le slot redevient vide).
    Clear {
        /// Index du slot à effacer (0 à 3)
        #[arg(long)]
        slot: u8,
    },
}

// ─── Point d'entrée ──────────────────────────────────────────────────────────

fn main() -> Result<()> {
    let cli = Cli::parse();

    // On extrait le port avant le match pour éviter que le déplacement de
    // cli.command n'empêche d'accéder à cli.port.
    let port = cli.port;

    match cli.command {
        Commands::List => {
            let port_path = require_port(&port)?;
            let mut transport = SerialTransport::open(port_path)?;
            cmd_list(&mut transport)
        }

        Commands::Provision { slot, label, secret, format, slot_type } => {
            let port_path = require_port(&port)?;
            let mut transport = SerialTransport::open(port_path)?;
            cmd_provision(&mut transport, slot, &label, &secret, format, &slot_type)
        }

        Commands::Clear { slot } => {
            let port_path = require_port(&port)?;
            let mut transport = SerialTransport::open(port_path)?;
            cmd_clear(&mut transport, slot)
        }
    }
}

// ─── Fonctions utilitaires ────────────────────────────────────────────────────

/// Retourne le chemin du port ou une erreur explicite si `--port` est absent.
fn require_port(port: &Option<String>) -> Result<&str> {
    match port {
        Some(path) => Ok(path.as_str()),
        None => bail!("--port est requis (ex: --port /dev/ttyACM1)"),
    }
}

/// Décode la valeur du secret depuis la chaîne fournie en CLI.
///
/// - `Hex`    : décodage hexadécimal standard
/// - `Base32` : décodage RFC4648 sans padding (les `=` finaux sont acceptés)
fn decode_secret(value: &str, format: SecretFormat) -> Result<Vec<u8>> {
    match format {
        SecretFormat::Hex => {
            hex::decode(value)
                .context("Décodage hex échoué — vérifiez que la valeur est en hexadécimal valide")
        }

        SecretFormat::Base32 => {
            // On normalise en majuscules et on supprime les `=` de padding
            // pour accepter aussi bien les seeds paddés que non-paddés.
            let normalized = value.trim_end_matches('=').to_uppercase();

            data_encoding::BASE32_NOPAD
                .decode(normalized.as_bytes())
                .context(
                    "Décodage base32 échoué — vérifiez que la valeur est en base32 RFC4648 valide",
                )
        }
    }
}

/// Traduit la chaîne `--type` de la CLI en [`SlotType`].
fn parse_slot_type_arg(s: &str) -> Result<SlotType> {
    match s.to_lowercase().as_str() {
        "hmac-sha1" => Ok(SlotType::HmacSha1),
        other => bail!(
            "Type de slot inconnu : '{}' (seul 'hmac-sha1' est supporté pour l'instant)",
            other
        ),
    }
}

/// Vérifie que la réponse du dongle indique un succès, sinon retourne une erreur.
///
/// Inclut le cmd de la réponse dans le message d'erreur pour faciliter le débogage.
fn check_status_ok(response: &cdc::Response) -> Result<()> {
    if response.status == status::OK {
        return Ok(());
    }

    bail!(
        "Le dongle a retourné une erreur pour la commande 0x{:02X} : {} (code 0x{:02X})",
        response.cmd,
        status::describe(response.status),
        response.status
    )
}

// ─── Implémentations des commandes ────────────────────────────────────────────

/// Exécute `list` : affiche les slots secrets provisionnés.
fn cmd_list(transport: &mut dyn Transport) -> Result<()> {
    // Construction de la requête SEC_LIST (payload vide)
    let payload = sec::list_payload();
    let request = build_request(sec::SEC_LIST, &payload);

    // Envoi et réception via le transport
    let raw_response = transport.transact(&request)?;

    // Décodage du frame KR
    let response = parse_response(&raw_response)?;
    check_status_ok(&response)?;

    // Décodage du payload de réponse
    let slots = sec::parse_list(&response.payload)?;

    if slots.is_empty() {
        println!("Aucun slot provisionné sur ce dongle.");
        return Ok(());
    }

    // Affichage en tableau
    println!("{:<6} {:<12} {}", "IDX", "TYPE", "LABEL");
    println!("{}", "-".repeat(38));
    for slot in &slots {
        println!(
            "{:<6} {:<12} {}",
            slot.idx,
            slot.slot_type.as_str(),
            slot.label
        );
    }

    Ok(())
}

/// Exécute `provision` : configure un slot avec un label et un secret.
fn cmd_provision(
    transport: &mut dyn Transport,
    slot: u8,
    label: &str,
    secret_encoded: &str,
    format: SecretFormat,
    slot_type_str: &str,
) -> Result<()> {
    // ── Décodage et validation des arguments ──────────────────────────────
    let slot_type = parse_slot_type_arg(slot_type_str)?;
    let secret_bytes = decode_secret(secret_encoded, format)?;

    // set_slot_payload valide aussi : idx, longueur label, longueur secret
    let payload = sec::set_slot_payload(slot, slot_type, label, &secret_bytes)?;

    // ── Envoi de la requête SEC_SET_SLOT ──────────────────────────────────
    let request = build_request(sec::SEC_SET_SLOT, &payload);
    let raw_response = transport.transact(&request)?;

    // ── Vérification du statut ────────────────────────────────────────────
    let response = parse_response(&raw_response)?;
    check_status_ok(&response)?;

    println!(
        "Slot {} provisionné avec succès (label : '{}', {} octets de secret).",
        slot,
        label,
        secret_bytes.len()
    );
    Ok(())
}

/// Exécute `clear` : efface un slot secret.
fn cmd_clear(transport: &mut dyn Transport, slot: u8) -> Result<()> {
    // Construction et envoi de la requête SEC_CLEAR_SLOT
    let payload = sec::clear_slot_payload(slot)?;
    let request = build_request(sec::SEC_CLEAR_SLOT, &payload);
    let raw_response = transport.transact(&request)?;

    // Vérification du statut
    let response = parse_response(&raw_response)?;
    check_status_ok(&response)?;

    println!("Slot {} effacé avec succès.", slot);
    Ok(())
}

// ─── Tests unitaires ─────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    // ── Mock Transport : simule le dongle sans matériel ───────────────────

    /// Transport de test qui retourne une réponse préprogrammée.
    struct MockTransport {
        /// Frame KR complet que le mock retourne à chaque appel de transact()
        response_frame: Vec<u8>,
    }

    impl MockTransport {
        /// Construit un mock qui retourne une réponse KR valide avec le payload donné.
        fn with_ok_response(cmd: u8, payload: &[u8]) -> Self {
            let crc = cdc::crc8(payload);
            let payload_len = payload.len() as u16;
            let len_bytes = payload_len.to_le_bytes();

            let mut frame = vec![
                0x4B, 0x52,     // magic KR
                cmd,            // commande miroir
                status::OK,     // statut OK
                len_bytes[0],   // len_lo
                len_bytes[1],   // len_hi
            ];
            frame.extend_from_slice(payload);
            frame.push(crc);

            MockTransport { response_frame: frame }
        }
    }

    impl Transport for MockTransport {
        fn transact(&mut self, _request: &[u8]) -> Result<Vec<u8>> {
            Ok(self.response_frame.clone())
        }
    }

    // ── Tests de cmd_list ─────────────────────────────────────────────────

    #[test]
    fn cmd_list_avec_liste_vide() {
        // Le dongle retourne count=0 → pas de slots
        let list_response_payload = vec![0x00u8]; // count = 0
        let mut transport = MockTransport::with_ok_response(sec::SEC_LIST, &list_response_payload);

        // cmd_list ne doit pas paniquer ni retourner une erreur
        let result = cmd_list(&mut transport);
        assert!(result.is_ok(), "cmd_list avec liste vide doit réussir");
    }

    #[test]
    fn cmd_list_avec_un_slot() {
        // Payload : count=1, slot idx=0, type=HmacSha1, label="test"
        let mut payload = vec![0x01u8]; // count = 1
        payload.push(0); // idx
        payload.push(1); // type HmacSha1
        let mut label_field = [0u8; 16];
        label_field[..4].copy_from_slice(b"test");
        payload.extend_from_slice(&label_field);

        let mut transport = MockTransport::with_ok_response(sec::SEC_LIST, &payload);
        let result = cmd_list(&mut transport);
        assert!(result.is_ok(), "cmd_list avec un slot doit réussir");
    }

    // ── Tests de cmd_provision ────────────────────────────────────────────

    #[test]
    fn cmd_provision_hex_valide() {
        // Réponse OK sans payload pour SEC_SET_SLOT
        let mut transport = MockTransport::with_ok_response(sec::SEC_SET_SLOT, &[]);

        let result = cmd_provision(
            &mut transport,
            0,
            "github",
            "deadbeef",        // 4 octets en hex
            SecretFormat::Hex,
            "hmac-sha1",
        );
        assert!(result.is_ok(), "provision avec hex valide doit réussir");
    }

    #[test]
    fn cmd_provision_base32_valide() {
        // "MFRA" en base32 RFC4648 = [0x61, 0x46, 0x80] (3 octets)
        let mut transport = MockTransport::with_ok_response(sec::SEC_SET_SLOT, &[]);

        let result = cmd_provision(
            &mut transport,
            1,
            "totp",
            "MFRA",
            SecretFormat::Base32,
            "hmac-sha1",
        );
        assert!(result.is_ok(), "provision avec base32 valide doit réussir");
    }

    #[test]
    fn cmd_provision_rejette_hex_invalide() {
        let mut transport = MockTransport::with_ok_response(sec::SEC_SET_SLOT, &[]);

        let result = cmd_provision(
            &mut transport,
            0,
            "test",
            "gg",              // 'g' n'est pas un caractère hex
            SecretFormat::Hex,
            "hmac-sha1",
        );
        assert!(result.is_err(), "hex invalide doit être rejeté");
    }

    #[test]
    fn cmd_provision_rejette_type_inconnu() {
        let mut transport = MockTransport::with_ok_response(sec::SEC_SET_SLOT, &[]);

        let result = cmd_provision(
            &mut transport,
            0,
            "test",
            "deadbeef",
            SecretFormat::Hex,
            "totp-sha256",    // type non supporté
        );
        assert!(result.is_err(), "type inconnu doit être rejeté");
    }

    // ── Tests de cmd_clear ────────────────────────────────────────────────

    #[test]
    fn cmd_clear_slot_valide() {
        let mut transport = MockTransport::with_ok_response(sec::SEC_CLEAR_SLOT, &[]);
        let result = cmd_clear(&mut transport, 2);
        assert!(result.is_ok(), "clear d'un slot valide doit réussir");
    }

    // ── Tests de decode_secret ────────────────────────────────────────────

    #[test]
    fn decode_secret_hex_correct() {
        let bytes = decode_secret("deadbeef", SecretFormat::Hex).unwrap();
        assert_eq!(bytes, vec![0xDE, 0xAD, 0xBE, 0xEF]);
    }

    #[test]
    fn decode_secret_base32_accepte_padding() {
        // "MFRA====" est identique à "MFRA" en base32 sans padding
        let with_pad    = decode_secret("MFRA====", SecretFormat::Base32).unwrap();
        let without_pad = decode_secret("MFRA",     SecretFormat::Base32).unwrap();
        assert_eq!(with_pad, without_pad, "base32 paddé et non paddé doivent donner le même résultat");
    }

    #[test]
    fn decode_secret_base32_minuscules_acceptees() {
        // Les seeds TOTP sont parfois en minuscules
        let lower = decode_secret("mfra", SecretFormat::Base32).unwrap();
        let upper = decode_secret("MFRA", SecretFormat::Base32).unwrap();
        assert_eq!(lower, upper, "base32 minuscules doit être équivalent à majuscules");
    }

    // ── Tests de require_port ─────────────────────────────────────────────

    #[test]
    fn require_port_some_retourne_chemin() {
        let port = Some("/dev/ttyACM1".to_string());
        let path = require_port(&port).unwrap();
        assert_eq!(path, "/dev/ttyACM1");
    }

    #[test]
    fn require_port_none_retourne_erreur() {
        let result = require_port(&None);
        assert!(result.is_err(), "--port absent doit retourner une erreur");
    }
}
