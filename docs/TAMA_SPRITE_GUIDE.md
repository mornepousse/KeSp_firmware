# Guide Sprites Tamagotchi — KaSe Keyboard

## Contraintes techniques

| Paramètre | Valeur |
|-----------|--------|
| **Taille sprite** | 32×32 pixels |
| **Couleurs** | Monochrome (1 bit : pixel ON ou OFF) |
| **Écran round** | 240×240 px — sprite affiché à ~80×80 px (zoom 2.5x) |
| **Écran OLED** | 128×64 px — sprite affiché à 32×32 px (taille native) |
| **Zone disponible** | ~100×100 px au centre de l'écran round |
| **Format de livraison** | PNG 32×32 monochrome (noir et blanc pur, pas de gris) |

## Ce qu'il faut dessiner

### 20 créatures (niveaux d'évolution)

Chaque créature a **2 poses** :
- **Main** : pose normale / active
- **Idle** : pose repos (légère variation — yeux fermés, posture détendue, etc.)

L'animation bascule entre les 2 poses toutes les 600ms + un léger mouvement vertical de respiration.

| Niveau | Nom suggéré | Style | Description |
|--------|-------------|-------|-------------|
| 1 | Egg | Simple | Un œuf avec un motif |
| 2-5 | Bébé | Petit, rond | Petite créature mignonne, simple |
| 6-10 | Enfant | Plus détaillé | Membres visibles, expression |
| 11-15 | Ado | Complexe | Personnalité distincte |
| 16-19 | Adulte | Détaillé | Design unique, élaboré |
| 20 | Ultime | Épique | Forme finale, impressionnante |

### 8 icônes d'état (16×16 pixels)

| Icône | Usage |
|-------|-------|
| Nourriture | Barre de faim |
| Cœur | Bonheur |
| Éclair | Énergie |
| Croix médicale | Santé/médecine |
| Étoile | Niveau/XP |
| Zzz | Sommeil |
| ! | Alerte (faim basse) |
| ☺ | Célébration |

## Règles de design

### Pixel art monochrome
- **Noir et blanc uniquement** — pas de niveaux de gris
- Le firmware colorise le sprite selon l'état émotionnel :
  - Blanc = idle
  - Vert = happy
  - Jaune = excited
  - Orange = eating
  - Rouge = sick
  - Bleu = sad
  - Violet foncé = sleeping
  - Magenta = celebrating

### Conseils
- **Centrer** la créature dans le cadre 32×32
- **Garder 2-3px de marge** sur les bords (le zoom peut couper les pixels extrêmes)
- Les **yeux** sont importants — c'est ce qui donne l'émotion
- La différence entre main et idle doit être **subtile** (1-3 pixels de changement) — l'animation est rapide (600ms)
- **Silhouette claire** — le sprite doit être reconnaissable même à 32×32

### Exemples de différence main/idle
```
Main :          Idle :
  ●  ●            —  —       (yeux ouverts → fermés)
  ╰──╯            ╰──╯

   /\              /\
  /  \            /  \        (même corps, légère rotation)
 /    \          /   \
```

## Livraison

### Format fichiers
```
sprites/
├── egg_main.png      (32×32, noir et blanc)
├── egg_idle.png
├── baby1_main.png
├── baby1_idle.png
├── ...
├── ultimate_main.png
├── ultimate_idle.png
├── icon_food.png     (16×16)
├── icon_heart.png
├── icon_bolt.png
├── icon_meds.png
├── icon_star.png
├── icon_sleep.png
├── icon_alert.png
└── icon_celebrate.png
```

### Validation
- Ouvrir dans un éditeur de pixel art (Aseprite, Piskel, GIMP)
- Vérifier que c'est bien 32×32 (ou 16×16 pour les icônes)
- Vérifier qu'il n'y a que 2 couleurs (noir pur #000000 et blanc pur #FFFFFF)
- Le fond doit être **noir** (#000000), la créature en **blanc** (#FFFFFF)

## Outils recommandés

| Outil | URL | Usage |
|-------|-----|-------|
| **Piskel** | piskelapp.com | Gratuit, en ligne, parfait pour pixel art |
| **Aseprite** | aseprite.org | Pro, payant, le meilleur pour l'animation |
| **GIMP** | gimp.org | Gratuit, pour retouches finales |
| **Lospec** | lospec.com/pixel-art-tutorials | Tutoriels pixel art |

## Contexte

Le sprite s'affiche au centre d'un écran rond de smartwatch (240×240 px). Il est entouré de :
- Un arc KPM (vitesse de frappe) sur le pourtour
- Le nom du layout clavier en haut
- Des barres de stats (faim, bonheur, énergie) en dessous
- Le niveau et nom de la créature en texte

Le tamagotchi vit grâce à l'utilisation du clavier — plus tu tapes, plus il est content. Il évolue en gagnant de l'XP par les frappes.
