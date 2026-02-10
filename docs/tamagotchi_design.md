# 🐱 Tamagotchi Clavier - Design Document

## 📋 Vue d'ensemble

Création d'un petit compagnon virtuel style Tamagotchi sur l'écran rond GC9A01 (240x240) qui réagit à l'utilisation du clavier.

### Objectifs
- Ajouter un élément ludique et attachant au clavier
- Réagir aux événements clavier (frappe, inactivité, vitesse de frappe)
- Utiliser le KPM existant pour influencer l'état du pet
- S'intégrer proprement à l'UI existante (`round_ui.c`)

---

## 🎨 Design Visuel

### Contraintes écran
- **Résolution** : 240x240 pixels (rond)
- **Zone utilisable** : ~200x200 (intérieur de l'arc)
- **Librairie** : LVGL
- **Style** : Dark theme existant avec accents cyan

### Éléments du personnage
```
+---------------------------+
|      [Status icons]       |
|        [Conn icon]        |
|                           |
|       ╭───────────╮       |
|       │  (• ◡ •)  │       |  <- Zone Tamagotchi
|       │    \_/    │       |
|       ╰───────────╯       |
|                           |
|        [Layer name]       |
|         [KPM/Stats]       |
+---------------------------+
```

### Sprites/États visuels (ASCII Art → Pixels ou Images)

| État | Expression | Déclencheur |
|------|------------|-------------|
| `IDLE` | `(• ◡ •)` | Pas d'activité depuis 30s |
| `HAPPY` | `(^ ◡ ^)` | KPM > 100 |
| `EXCITED` | `(★ ω ★)` | KPM > 200 |
| `SLEEPY` | `(- _ -)` | Inactivité > 2min |
| `SLEEPING` | `(- _ -)zzZ` | Inactivité > 5min |
| `WORKING` | `(• _ •)` | Activité normale |
| `TIRED` | `(• . •)` | Longue session > 30min |
| `CELEBRATING` | `\(^o^)/` | Milestone atteint |

---

## 🔧 Architecture Technique

### Nouveau module : `tamagotchi.c` / `tamagotchi.h`

```c
// États du Tamagotchi
typedef enum {
    TAMA_STATE_IDLE,
    TAMA_STATE_HAPPY,
    TAMA_STATE_EXCITED,
    TAMA_STATE_WORKING,
    TAMA_STATE_SLEEPY,
    TAMA_STATE_SLEEPING,
    TAMA_STATE_TIRED,
    TAMA_STATE_CELEBRATING
} tama_state_t;

// Stats persistantes (à sauvegarder en NVS)
typedef struct {
    uint32_t total_keypresses;      // Total depuis création
    uint32_t session_keypresses;    // Session actuelle
    uint32_t max_kpm_ever;          // Record personnel
    uint32_t days_active;           // Jours d'utilisation
    uint16_t happiness;             // 0-1000
    uint16_t energy;                // 0-1000
    uint8_t level;                  // Niveau du pet (évolution?)
} tama_stats_t;

// API publique
void tamagotchi_init(void);
void tamagotchi_update(uint32_t kpm);
void tamagotchi_notify_keypress(void);
void tamagotchi_draw(lv_obj_t *parent);
tama_state_t tamagotchi_get_state(void);
const tama_stats_t* tamagotchi_get_stats(void);
void tamagotchi_save_stats(void);    // Sauvegarde NVS
void tamagotchi_load_stats(void);    // Chargement NVS
```

### Intégration avec `round_ui.c`

```c
// Dans round_ui_init()
tamagotchi_init();
tamagotchi_draw(main_container);

// Dans round_ui_update()
tamagotchi_update(current_kpm);

// Dans round_ui_notify_keypress()
tamagotchi_notify_keypress();
```

---

## 📊 Logique de Comportement

### Machine à états

```
                    ┌─────────────┐
                    │   SLEEPING  │
                    └──────┬──────┘
                           │ keypress
                           ▼
┌─────────┐  inactivité   ┌─────────┐
│  IDLE   │◄──────────────│ WORKING │
└────┬────┘    30s        └────┬────┘
     │                         │
     │ inactivité 2min         │ KPM > 100
     ▼                         ▼
┌─────────┐               ┌─────────┐
│ SLEEPY  │               │  HAPPY  │
└────┬────┘               └────┬────┘
     │                         │
     │ inactivité 5min         │ KPM > 200
     ▼                         ▼
┌─────────┐               ┌─────────┐
│SLEEPING │               │ EXCITED │
└─────────┘               └─────────┘
```

### Paramètres de transition

```c
#define TAMA_IDLE_TIMEOUT_MS        (30 * 1000)     // 30 secondes
#define TAMA_SLEEPY_TIMEOUT_MS      (2 * 60 * 1000) // 2 minutes
#define TAMA_SLEEP_TIMEOUT_MS       (5 * 60 * 1000) // 5 minutes
#define TAMA_KPM_HAPPY_THRESHOLD    100
#define TAMA_KPM_EXCITED_THRESHOLD  200
#define TAMA_SESSION_TIRED_MS       (30 * 60 * 1000) // 30 minutes
```

---

## 🖼️ Options de Rendu

### Option 1 : Labels LVGL avec caractères Unicode
```c
// Simple et léger
lv_label_set_text(tama_face, "(• ◡ •)");
```

**Avantages** : Très simple, pas d'assets
**Inconvénients** : Limité visuellement, dépend de la font

### Option 2 : Images/Sprites LVGL
```c
// Sprites pixel art converties
LV_IMG_DECLARE(tama_idle);
LV_IMG_DECLARE(tama_happy);
lv_img_set_src(tama_img, &tama_happy);
```

**Avantages** : Joli, animations fluides possibles
**Inconvénients** : Plus de mémoire flash, création des assets

### Option 3 : Canvas LVGL (dessin procédural)
```c
// Dessiner le personnage avec des primitives
lv_canvas_draw_arc(canvas, ...);  // Visage
lv_canvas_draw_circle(canvas, ...);  // Yeux
```

**Avantages** : Flexible, animations procédurales
**Inconvénients** : Plus complexe à coder

### Recommandation : **Option 2 (Images)** pour un résultat joli, ou **Option 1 (Labels)** pour commencer rapidement

---

## 🎬 Animations

### Animations possibles avec LVGL

1. **Idle breathing** : Légère oscillation verticale
   ```c
   lv_anim_t a;
   lv_anim_set_var(&a, tama_container);
   lv_anim_set_values(&a, 0, 5);
   lv_anim_set_exec_cb(&a, anim_y_cb);
   lv_anim_set_time(&a, 2000);
   lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
   lv_anim_set_playback_time(&a, 2000);
   ```

2. **Keypress bounce** : Petit saut à chaque touche
3. **State transition** : Fondu entre expressions
4. **Celebration dance** : Rotation + scale pour milestones

---

## 💾 Persistence (NVS)

### Données à sauvegarder

```c
// Clé NVS : "tama_stats"
nvs_set_blob(handle, "tama_stats", &stats, sizeof(tama_stats_t));

// Sauvegarder périodiquement (toutes les 5 minutes)
// Et à l'arrêt propre si possible
```

---

## 📅 Plan d'implémentation

### Phase 1 : MVP (Version simple)
- [ ] Créer `tamagotchi.h` avec structures de base
- [ ] Créer `tamagotchi.c` avec machine à états simple
- [ ] Affichage texte ASCII du personnage
- [ ] Intégration basique dans `round_ui.c`
- [ ] Réaction au KPM existant

### Phase 2 : Polish visuel
- [ ] Créer sprites pixel art (ou trouver assets libres)
- [ ] Implémenter affichage image LVGL
- [ ] Ajouter animation idle (breathing)
- [ ] Animation de transition entre états

### Phase 3 : Gamification
- [ ] Système de stats persistant (NVS)
- [ ] Milestones et célébrations
- [ ] Système de "level up" basé sur l'utilisation
- [ ] Peut-être : évolution visuelle du personnage

### Phase 4 : Features avancées
- [ ] Plusieurs personnages au choix
- [ ] Mini-jeux débloquables ?
- [ ] Intégration avec LED strip (couleur selon humeur)
- [ ] Personnalisation via config

---

## 📁 Structure de fichiers

```
main/display/
├── tamagotchi.c          # Logique du Tamagotchi
├── tamagotchi.h          # API publique
├── tama_sprites.c        # Sprites LVGL (généré)
├── tama_sprites.h        # Déclarations sprites
└── round_ui.c            # (modifier pour intégration)
```

---

## 🎮 Idées futures

- **Mode combat** : Comparer stats avec d'autres claviers via NRF24 ?
- **Achievements** : Badges pour jalons (10k touches, 300 KPM, etc.)
- **Saisonnalité** : Apparence différente selon la date
- **Easter eggs** : Réactions à certaines séquences de touches

---

## Références

- [LVGL Image Converter](https://lvgl.io/tools/imageconverter)
- [LVGL Animation docs](https://docs.lvgl.io/master/overview/animation.html)
- [Tamagotchi sprites inspiration](https://www.spriters-resource.com/)
