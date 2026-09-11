/* Poignée de main 5 V du lien inter-moitiés.
 *
 * Enjeu matériel : fermer le load switch avant d'avoir reconnu la moitié d'en
 * face, c'est mettre du 5 V sur un connecteur exposé — exactement le tueur
 * historique des splits que ce design cherche à éliminer. La règle testée ici
 * est donc : LINK_5V_EN ne se lève JAMAIS sans une reconnaissance aboutie, et il
 * retombe dès que la moitié d'en face se tait.
 */
#include <string.h>
#include "test_framework.h"
#include "../main/comm/link/link_handshake.h"

static void test_starts_dead(void)
{
    link_hs_t hs;
    link_hs_init(&hs);
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "au repos au départ");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V mort au départ");
}

static void test_probe_then_ack_enables_5v(void)
{
    link_hs_t hs;
    link_hs_init(&hs);

    /* On a l'USB : on sonde. */
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 1000);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_SEND_PROBE, "présence USB → sonder");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V toujours mort pendant la sonde");

    /* La moitié d'en face répond. */
    a = link_hs_step(&hs, LINK_HS_EV_PEER_ACK, 1050);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_ENABLE_5V, "pair reconnu → fermer le switch");
    TEST_ASSERT(link_hs_5v_enabled(&hs), "5 V vivant après reconnaissance");
    TEST_ASSERT_EQ(hs.state, LINK_HS_UP, "lien établi");
}

static void test_never_enables_without_ack(void)
{
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);

    /* Du bruit sur la ligne, pas un ACK : rien ne doit se lever. */
    for (uint32_t t = 1; t < LINK_HS_PROBE_TIMEOUT_MS; t += 10) {
        link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, t);
        TEST_ASSERT(a != LINK_HS_ACT_ENABLE_5V, "aucun 5 V sans reconnaissance");
        TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V reste mort");
    }
}

static void test_probe_timeout_returns_to_idle(void)
{
    link_hs_t hs;
    link_hs_init(&hs);

    /* Précondition du test : on doit être réellement en PROBING avant le
     * timeout, sinon un stub qui ignorerait USB_PRESENT passerait ce test
     * pour de mauvaises raisons (NONE/IDLE/5V-mort sont aussi sa sortie). */
    link_hs_action_t a0 = link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    TEST_ASSERT_EQ(a0, LINK_HS_ACT_SEND_PROBE, "présence USB → sonder");
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "en sonde avant le timeout");

    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, LINK_HS_PROBE_TIMEOUT_MS);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "pas de réponse → on abandonne");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "retour au repos");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V mort après abandon");
}

static void test_peer_ack_ignored_in_idle(void)
{
    /* Un ACK non sollicité, avant toute sonde : on ne l'a jamais demandé, on
     * ne le croit pas. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_PEER_ACK, 5);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "ACK non sollicité en IDLE → rien");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "reste au repos");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V mort");
}

static void test_peer_frame_ignored_in_probing(void)
{
    /* Une trame reçue avant l'ACK ne doit pas faire transitionner : seul
     * PEER_ACK vaut reconnaissance de la moitié d'en face. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_PEER_FRAME, 10);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "trame sans ACK en PROBING → rien");
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "reste en sonde");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V mort");
}

static void test_usb_gone_already_dead_from_idle(void)
{
    /* USB_GONE alors que le 5 V est déjà mort (jamais monté) : pas de
     * DISABLE_5V redondant, mais pas de crash ni d'état incohérent non plus. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_USB_GONE, 5);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "5 V déjà mort en IDLE → pas de DISABLE_5V");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "reste au repos");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V mort");
}

static void test_usb_gone_already_dead_from_probing(void)
{
    /* Même chose depuis PROBING : la sonde est partie mais le 5 V n'est pas
     * encore levé, donc USB_GONE ne doit pas prétendre l'avoir coupé.
     *
     * Précondition vérifiée explicitement : sans elle, ce test passerait
     * identiquement contre un stub où USB_PRESENT ne ferait rien (la machine
     * resterait en IDLE) — l'état final (IDLE, 5 V mort) serait le même sans
     * être jamais passé par PROBING, et le nom du test mentirait. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_action_t a0 = link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    TEST_ASSERT_EQ(a0, LINK_HS_ACT_SEND_PROBE, "présence USB → sonder");
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "en sonde avant USB_GONE");

    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_USB_GONE, 10);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "5 V déjà mort en PROBING → pas de DISABLE_5V");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "retour au repos");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V mort");
}

static void test_late_ack_after_timeout_still_accepted(void)
{
    /* Comportement épinglé, pas un bug : si aucun TICK n'a fait retomber
     * PROBING avant l'arrivée de l'ACK — même après le délai nominal — l'ACK
     * reste requis et est accepté. Accepter un ACK tardif vaut mieux que
     * forcer une re-sonde. Ne pas changer ce comportement sans décision
     * explicite ; ce test l'épingle. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);

    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_PEER_ACK,
                                       LINK_HS_PROBE_TIMEOUT_MS + 50);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_ENABLE_5V, "ACK tardif encore accepté (épinglé)");
    TEST_ASSERT(link_hs_5v_enabled(&hs), "5 V levé malgré le retard");
    TEST_ASSERT_EQ(hs.state, LINK_HS_UP, "lien établi malgré le retard");
}

static void test_unknown_state_falls_safe_without_init(void)
{
    /* Contrat d'appel violé : struct sur pile jamais passée par
     * link_hs_init(), remplie de mémoire arbitraire (ici 0xFF partout). Le
     * `default:` du switch doit retomber côté sûr — 5 V mort — plutôt que de
     * préserver un en_5v de poubelle qui lirait `true` sans qu'aucun
     * ACT_ENABLE_5V n'ait jamais été émis. */
    link_hs_t hs;
    memset(&hs, 0xFF, sizeof(hs));
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, 0);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "état inconnu → aucune action");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "état inconnu → 5 V retombe mort");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "état inconnu → repli sur IDLE");
}

static void test_peer_silence_drops_5v(void)
{
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    link_hs_step(&hs, LINK_HS_EV_PEER_ACK, 10);
    TEST_ASSERT(link_hs_5v_enabled(&hs), "5 V vivant");

    /* Le pair se tait : câble arraché. Le switch doit rouvrir. */
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, 10 + LINK_HS_PEER_TIMEOUT_MS);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_DISABLE_5V, "silence du pair → rouvrir");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V mort après arrachage");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "retour au repos");
}

static void test_peer_traffic_keeps_link_up(void)
{
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    link_hs_step(&hs, LINK_HS_EV_PEER_ACK, 10);

    /* Trafic régulier : le lien tient indéfiniment. */
    for (uint32_t t = 10; t < 10 * LINK_HS_PEER_TIMEOUT_MS; t += LINK_HS_PEER_TIMEOUT_MS / 2) {
        link_hs_step(&hs, LINK_HS_EV_PEER_FRAME, t);
        TEST_ASSERT(link_hs_5v_enabled(&hs), "le trafic maintient le lien");
    }
}

static void test_usb_lost_drops_5v(void)
{
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    link_hs_step(&hs, LINK_HS_EV_PEER_ACK, 10);

    /* Débranché : on ne nourrit plus personne. */
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_USB_GONE, 20);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_DISABLE_5V, "USB parti → rouvrir");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V mort sans USB");
}

static void test_no_probe_without_usb(void)
{
    /* Sur batterie seule, on ne sonde pas : on n'a rien à donner. Même après
     * une longue attente — plusieurs multiples de l'intervalle de re-sonde —
     * l'absence d'USB doit rester la seule raison qui bloque tout. */
    link_hs_t hs;
    link_hs_init(&hs);
    for (uint32_t t = 0; t <= 5 * LINK_HS_REPROBE_INTERVAL_MS; t += 100) {
        link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, t);
        TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "pas d'USB → jamais de sonde, même après un long moment");
        TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "on reste au repos");
    }
}

static void test_lost_probe_eventually_reprobes(void)
{
    /* Un ACK perdu ne doit pas condamner le lien jusqu'au rebranchement du
     * câble : USB_PRESENT est un événement de front, il ne reviendra pas
     * tout seul. IDLE doit re-sonder tant que h->usb reste vrai. */
    link_hs_t hs;
    link_hs_init(&hs);

    link_hs_action_t a0 = link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    TEST_ASSERT_EQ(a0, LINK_HS_ACT_SEND_PROBE, "présence USB → première sonde");
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "en sonde");

    /* La sonde se perd : timeout, retour à IDLE sans avoir jamais levé le 5V. */
    link_hs_action_t a1 = link_hs_step(&hs, LINK_HS_EV_TICK, LINK_HS_PROBE_TIMEOUT_MS);
    TEST_ASSERT_EQ(a1, LINK_HS_ACT_NONE, "timeout de sonde → abandon");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "retour au repos");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V toujours mort");

    /* Pas encore l'heure de re-sonder : l'intervalle n'est pas écoulé. */
    link_hs_action_t a2 = link_hs_step(&hs, LINK_HS_EV_TICK, LINK_HS_PROBE_TIMEOUT_MS + 1);
    TEST_ASSERT_EQ(a2, LINK_HS_ACT_NONE, "pas encore l'heure de re-sonder");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "toujours au repos");

    /* L'intervalle de re-sonde est écoulé depuis l'entrée en IDLE : la
     * machine reparte d'elle-même, sans nouvel événement USB_PRESENT. */
    uint32_t t_reprobe = LINK_HS_PROBE_TIMEOUT_MS + LINK_HS_REPROBE_INTERVAL_MS;
    link_hs_action_t a3 = link_hs_step(&hs, LINK_HS_EV_TICK, t_reprobe);
    TEST_ASSERT_EQ(a3, LINK_HS_ACT_SEND_PROBE, "sonde perdue → re-sonde automatique après l'intervalle");
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "repart en sonde");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "sonder n'est pas lever le 5 V");
}

static void test_probe_timeout_wraps_uint32_cleanly(void)
{
    /* link_hs_step gère le débordement uint32 de now_ms via une soustraction
     * non signée. On n'a pas de test qui force ce chemin — un futur
     * "correctif" du genre `if (now_ms < since_ms) return;` casserait la
     * propriété silencieusement. since_ms proche de UINT32_MAX, now_ms après
     * le passage à zéro. */
    link_hs_t hs;
    link_hs_init(&hs);
    hs.usb = true;
    hs.state = LINK_HS_PROBING;
    hs.since_ms = 0xFFFFFFF0u; /* entrée en PROBING juste avant le débordement */

    /* now_ms a débordé (wrap) mais l'écart réel n'est que de 199 ms :
     * 0xFFFFFFF0 + 199 déborde à 183. Pas encore le timeout. */
    uint32_t elapsed_199 = (uint32_t)(0xFFFFFFF0u + (LINK_HS_PROBE_TIMEOUT_MS - 1));
    link_hs_action_t a0 = link_hs_step(&hs, LINK_HS_EV_TICK, elapsed_199);
    TEST_ASSERT_EQ(a0, LINK_HS_ACT_NONE, "199 ms après le wrap → pas encore le timeout");
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "toujours en sonde");

    /* Exactement 200 ms plus tard (toujours après le wrap) : le timeout doit
     * se déclencher normalement, malgré now_ms < since_ms en arithmétique
     * signée. */
    hs.since_ms = 0xFFFFFFF0u;
    hs.state = LINK_HS_PROBING;
    uint32_t elapsed_200 = (uint32_t)(0xFFFFFFF0u + LINK_HS_PROBE_TIMEOUT_MS);
    link_hs_action_t a1 = link_hs_step(&hs, LINK_HS_EV_TICK, elapsed_200);
    TEST_ASSERT_EQ(a1, LINK_HS_ACT_NONE, "timeout de sonde déclenché malgré le wrap de now_ms");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "retour au repos malgré le wrap");
}

/* ── I7 : le versant RÉCEPTEUR de la poignée de main ──────────────────────────
 *
 * Le contrat matériel exige que les DEUX moitiés ferment leur switch pour qu'un
 * courant passe (NIPHARGUS_V2_HARDWARE.md). La machine ne modélisait que
 * l'émetteur : la moitié sur batterie, elle, ne fermait jamais le sien, si bien
 * qu'une poignée de main réussie ne faisait passer aucun courant.
 *
 * L'invariant de sûreté devient : en_5v ne passe à vrai qu'après un ÉCHANGE
 * VÉRIFIÉ avec le pair — soit on a sondé et reçu un ACK, soit on a été sondé et
 * on a répondu. Un seul switch fermé ne fait toujours rien passer.
 */
static void test_probed_answers_and_closes_its_own_switch(void)
{
    link_hs_t hs;
    link_hs_init(&hs);

    /* Sur batterie, sans USB : c'est le pair qui sonde. */
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_PROBED, 100);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_ACK_AND_ENABLE_5V,
                   "sondée → elle répond ET ferme son switch");
    TEST_ASSERT(link_hs_5v_enabled(&hs), "son côté est fermé, le courant peut passer");
    TEST_ASSERT_EQ(hs.state, LINK_HS_UP, "le pair est reconnu vivant");
}

static void test_probed_while_probing_still_pairs(void)
{
    /* Les deux moitiés branchées se sondent en même temps : chacune reçoit la
     * sonde de l'autre alors qu'elle attend un ACK. Le lien doit s'établir quand
     * même plutôt que de rester bloqué en attente mutuelle. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "on attend un ACK");

    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_PROBED, 10);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_ACK_AND_ENABLE_5V, "sonde croisée → on répond et on ferme");
    TEST_ASSERT_EQ(hs.state, LINK_HS_UP, "le lien s'établit malgré le croisement");
}

static void test_probed_side_drops_5v_when_peer_goes_silent(void)
{
    /* Le récepteur n'a pas d'USB : il ne recevra jamais d'USB_GONE. Son seul
     * moyen de savoir que le câble est parti est le silence du pair. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_PROBED, 0);
    TEST_ASSERT(link_hs_5v_enabled(&hs), "fermé après la sonde");

    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, LINK_HS_PEER_TIMEOUT_MS);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_DISABLE_5V, "silence du pair → on rouvre");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V mort après arrachage du jack");
}

static void test_repeated_probes_keep_the_link_alive(void)
{
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_PROBED, 0);
    /* Le pair re-sonde périodiquement : chaque sonde vaut signe de vie. */
    for (uint32_t t = 0; t < 5 * LINK_HS_PEER_TIMEOUT_MS; t += LINK_HS_PEER_TIMEOUT_MS / 2) {
        link_hs_step(&hs, LINK_HS_EV_PROBED, t);
        TEST_ASSERT(link_hs_5v_enabled(&hs), "les sondes répétées maintiennent le lien");
    }
}

static void test_tick_alone_never_closes_the_switch(void)
{
    /* L'invariant, reformulé : sans le moindre échange avec le pair — ni ACK
     * reçu, ni sonde reçue — rien ne doit fermer le switch, quel que soit le
     * temps écoulé. */
    link_hs_t hs;
    link_hs_init(&hs);
    hs.usb = true;   /* même avec du courant à donner */
    for (uint32_t t = 0; t < 10 * LINK_HS_REPROBE_INTERVAL_MS; t += 37) {
        link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, t);
        TEST_ASSERT(a != LINK_HS_ACT_ENABLE_5V && a != LINK_HS_ACT_ACK_AND_ENABLE_5V,
                    "aucun tick ne ferme le switch sans échange avec le pair");
        TEST_ASSERT(!link_hs_5v_enabled(&hs), "5 V reste mort");
    }
}

/* ── Entretien du lien en UP ─────────────────────────────────────────────────
 *
 * Constate au banc le 2026-09-11, premiere fermeture du 5 V : le lien montait,
 * puis retombait 500 ms plus tard. En UP, l'emetteur ne sondait plus, le
 * recepteur n'avait donc plus rien a acquitter, et LINK_HS_PEER_TIMEOUT_MS
 * expirait. Le design comptait sur les trames MATRIX du chemin filaire pour
 * entretenir le lien — chemin qui n'existe pas, les moities parlent radio.
 *
 * Meme motif que trois pannes de septembre : « emettre sur evenement » et
 * « relacher sur silence » ne composent pas. Remede identique : tant qu'on a
 * du courant a donner, on le dit periodiquement. */

static void test_up_avec_usb_resonde_periodiquement(void)
{
    link_hs_t h; link_hs_init(&h);
    link_hs_step(&h, LINK_HS_EV_USB_PRESENT, 1000);          /* -> PROBING */
    link_hs_step(&h, LINK_HS_EV_PEER_ACK, 1010);             /* -> UP */
    TEST_ASSERT(h.state == LINK_HS_UP && h.en_5v, "lien monte");
    TEST_ASSERT(link_hs_step(&h, LINK_HS_EV_TICK, 1100) == LINK_HS_ACT_NONE,
                "pas encore : la periode n'est pas ecoulee");
    TEST_ASSERT(link_hs_step(&h, LINK_HS_EV_TICK, 1010 + LINK_HS_KEEPALIVE_MS)
                == LINK_HS_ACT_SEND_PROBE, "periode ecoulee : on resonde");
    TEST_ASSERT(h.state == LINK_HS_UP && h.en_5v, "et on RESTE en UP, 5 V ferme");
}

static void test_ack_en_up_entretient_le_lien(void)
{
    /* LE test de ce fichier cote banc : sonde + ACK toutes les periodes doivent
     * tenir le lien indefiniment, bien au-dela de PEER_TIMEOUT. */
    link_hs_t h; link_hs_init(&h);
    link_hs_step(&h, LINK_HS_EV_USB_PRESENT, 0);
    link_hs_step(&h, LINK_HS_EV_PEER_ACK, 10);
    uint32_t t = 10;
    for (int i = 0; i < 20; i++) {
        t += LINK_HS_KEEPALIVE_MS;
        TEST_ASSERT(link_hs_step(&h, LINK_HS_EV_TICK, t) == LINK_HS_ACT_SEND_PROBE, "resonde");
        TEST_ASSERT(link_hs_step(&h, LINK_HS_EV_PEER_ACK, t + 5) == LINK_HS_ACT_NONE, "ACK recu");
        TEST_ASSERT(h.state == LINK_HS_UP && h.en_5v, "toujours UP apres des secondes");
    }
}

static void test_up_sans_usb_ne_sonde_pas(void)
{
    /* Le recepteur (pas d'USB) est monte parce qu'on l'a sonde ; lui ne sonde
     * jamais — il n'a pas de courant a donner. */
    link_hs_t h; link_hs_init(&h);
    link_hs_step(&h, LINK_HS_EV_PROBED, 1000);               /* -> UP, en repondant */
    TEST_ASSERT(h.state == LINK_HS_UP && !h.usb, "recepteur monte, sans USB");
    TEST_ASSERT(link_hs_step(&h, LINK_HS_EV_TICK, 1000 + LINK_HS_KEEPALIVE_MS)
                == LINK_HS_ACT_NONE, "il ne sonde pas");
    TEST_ASSERT(link_hs_step(&h, LINK_HS_EV_TICK, 1000 + LINK_HS_PEER_TIMEOUT_MS)
                == LINK_HS_ACT_DISABLE_5V, "et il expire si plus personne ne le sonde");
}

static void test_la_periode_tient_sous_le_timeout(void)
{
    TEST_ASSERT(LINK_HS_KEEPALIVE_MS * 2 < LINK_HS_PEER_TIMEOUT_MS,
                "deux entretiens tiennent dans le delai d'expiration du pair");
}

void test_link_handshake(void)
{
    test_up_avec_usb_resonde_periodiquement();
    test_ack_en_up_entretient_le_lien();
    test_up_sans_usb_ne_sonde_pas();
    test_la_periode_tient_sous_le_timeout();
    printf("\n-- poignée de main 5 V du lien --\n");
    test_starts_dead();
    test_probed_answers_and_closes_its_own_switch();
    test_probed_while_probing_still_pairs();
    test_probed_side_drops_5v_when_peer_goes_silent();
    test_repeated_probes_keep_the_link_alive();
    test_tick_alone_never_closes_the_switch();
    test_probe_then_ack_enables_5v();
    test_never_enables_without_ack();
    test_probe_timeout_returns_to_idle();
    test_peer_silence_drops_5v();
    test_peer_traffic_keeps_link_up();
    test_usb_lost_drops_5v();
    test_no_probe_without_usb();
    test_peer_ack_ignored_in_idle();
    test_peer_frame_ignored_in_probing();
    test_usb_gone_already_dead_from_idle();
    test_usb_gone_already_dead_from_probing();
    test_late_ack_after_timeout_still_accepted();
    test_unknown_state_falls_safe_without_init();
    test_lost_probe_eventually_reprobes();
    test_probe_timeout_wraps_uint32_cleanly();
}
