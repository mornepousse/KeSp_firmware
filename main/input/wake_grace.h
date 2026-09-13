#pragma once
#include <stdint.h>

/* Grâce à laisser au pilote keyboard_button, recréé au réveil, avant de
 * conclure qu'une touche capturée a été relâchée « avant le premier balayage ».
 *
 * Le pilote ne rapporte un PRESSED qu'après `debounce_ticks` balayages
 * consécutifs espacés de `interval_us` (keyboard_button.c, CALL_EVENT_CB sur
 * changement seulement). L'ancien code attendait `vTaskDelay(1)` en croyant
 * attendre 10 ms — or un tick nu attend jusqu'à la PROCHAINE frontière de tick,
 * soit n'importe quoi entre ~0 et 10 ms : quand la phase tombait mal, le pilote
 * n'avait pas fini son anti-rebond, son silence était pris pour un relâchement,
 * et une touche TENUE était relâchée au dongle 20 ms après le réveil (banc
 * 2026-09-13 : Super tenu → tap de Super vu par l'hôte → lanceur ouvert →
 * Super+F perdu).
 *
 * Formule : anti-rebond + 2 balayages (démarrage de la tâche, phase du timer)
 * + 10 ms de marge de création, plancher 10 ms (jamais moins qu'avant),
 * plafond 50 ms (un pilote muet ne retarde pas le réveil au-delà). L'appelant
 * sort AVANT l'échéance dès que le pilote a parlé : la grâce ne coûte son plein
 * que si la touche a réellement été relâchée.
 *
 * Pure, testée host (test/test_wake_grace.c). */
static inline uint32_t wake_grace_ms(uint32_t debounce_ticks, uint32_t interval_us)
{
    uint32_t anti_rebond_ms = ((debounce_ticks + 2u) * interval_us + 999u) / 1000u;
    uint32_t ms = anti_rebond_ms + 10u;
    if (ms < 10u) ms = 10u;
    if (ms > 50u) ms = 50u;
    return ms;
}
