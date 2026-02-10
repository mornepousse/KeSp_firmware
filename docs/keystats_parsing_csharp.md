# Parsing KEYSTATS depuis CDC ACM en C#

## Format du protocole

### Commandes disponibles

| Commande | Description |
|----------|-------------|
| `KEYSTATS` | Retourne les stats en binaire (pour heatmap) |
| `KEYSTATS?` | Retourne les stats en texte lisible |
| `KEYSTATS_RESET` | Remet les compteurs à zéro |

### Format binaire de `KEYSTATS`

```
┌──────────────────────────────────────────────────────────┐
│ HEADER (18 bytes)                                        │
├──────────────┬───────────────────────────────────────────┤
│ Offset 0-7   │ "KEYSTATS" (8 bytes ASCII)                │
│ Offset 8     │ rows (1 byte) = 5                         │
│ Offset 9     │ cols (1 byte) = 13                        │
│ Offset 10-13 │ total_presses (uint32 little-endian)      │
│ Offset 14-17 │ max_presses (uint32 little-endian)        │
├──────────────┴───────────────────────────────────────────┤
│ DATA (rows × cols × 4 bytes = 260 bytes)                 │
├──────────────────────────────────────────────────────────┤
│ Pour chaque [row][col]: uint32 LE = nombre de pressions  │
│ Ordre: [0][0], [0][1], ..., [0][12], [1][0], ..., [4][12]│
└──────────────────────────────────────────────────────────┘
```

## Code C# de parsing

```csharp
using System;
using System.IO.Ports;
using System.Text;
using System.Threading;

namespace KeyboardHeatmap
{
    public class KeyStats
    {
        public int Rows { get; set; }
        public int Cols { get; set; }
        public uint TotalPresses { get; set; }
        public uint MaxPresses { get; set; }
        public uint[,] PressCount { get; set; }
        
        /// <summary>
        /// Retourne l'intensité normalisée (0.0 - 1.0) pour une touche
        /// </summary>
        public double GetIntensity(int row, int col)
        {
            if (MaxPresses == 0) return 0;
            return (double)PressCount[row, col] / MaxPresses;
        }
        
        /// <summary>
        /// Retourne une couleur de heatmap (bleu froid -> rouge chaud)
        /// </summary>
        public (byte R, byte G, byte B) GetHeatmapColor(int row, int col)
        {
            double intensity = GetIntensity(row, col);
            
            // Gradient: Bleu (froid) -> Cyan -> Vert -> Jaune -> Rouge (chaud)
            byte r, g, b;
            
            if (intensity < 0.25)
            {
                // Bleu -> Cyan
                double t = intensity / 0.25;
                r = 0;
                g = (byte)(255 * t);
                b = 255;
            }
            else if (intensity < 0.5)
            {
                // Cyan -> Vert
                double t = (intensity - 0.25) / 0.25;
                r = 0;
                g = 255;
                b = (byte)(255 * (1 - t));
            }
            else if (intensity < 0.75)
            {
                // Vert -> Jaune
                double t = (intensity - 0.5) / 0.25;
                r = (byte)(255 * t);
                g = 255;
                b = 0;
            }
            else
            {
                // Jaune -> Rouge
                double t = (intensity - 0.75) / 0.25;
                r = 255;
                g = (byte)(255 * (1 - t));
                b = 0;
            }
            
            return (r, g, b);
        }
    }

    public class KeyStatsReader
    {
        private SerialPort _port;
        
        public KeyStatsReader(string portName, int baudRate = 115200)
        {
            _port = new SerialPort(portName, baudRate);
            _port.ReadTimeout = 2000;
            _port.WriteTimeout = 1000;
        }
        
        public void Open() => _port.Open();
        public void Close() => _port.Close();
        
        /// <summary>
        /// Récupère les statistiques de touches en binaire
        /// </summary>
        public KeyStats GetKeyStats()
        {
            // Vider le buffer
            _port.DiscardInBuffer();
            
            // Envoyer la commande
            _port.WriteLine("KEYSTATS");
            
            // Attendre un peu pour la réponse
            Thread.Sleep(100);
            
            // Lire le header (18 bytes)
            byte[] header = new byte[18];
            int read = 0;
            while (read < 18)
            {
                read += _port.Read(header, read, 18 - read);
            }
            
            // Vérifier le magic
            string magic = Encoding.ASCII.GetString(header, 0, 8);
            if (magic != "KEYSTATS")
            {
                throw new Exception($"Invalid magic: {magic}");
            }
            
            // Parser le header
            int rows = header[8];
            int cols = header[9];
            uint totalPresses = BitConverter.ToUInt32(header, 10);
            uint maxPresses = BitConverter.ToUInt32(header, 14);
            
            // Lire les données (rows * cols * 4 bytes)
            int dataSize = rows * cols * 4;
            byte[] data = new byte[dataSize];
            read = 0;
            while (read < dataSize)
            {
                read += _port.Read(data, read, dataSize - read);
            }
            
            // Parser les données
            uint[,] pressCount = new uint[rows, cols];
            int offset = 0;
            for (int r = 0; r < rows; r++)
            {
                for (int c = 0; c < cols; c++)
                {
                    pressCount[r, c] = BitConverter.ToUInt32(data, offset);
                    offset += 4;
                }
            }
            
            // Lire la fin de ligne (CRLF)
            _port.ReadLine();
            
            return new KeyStats
            {
                Rows = rows,
                Cols = cols,
                TotalPresses = totalPresses,
                MaxPresses = maxPresses,
                PressCount = pressCount
            };
        }
        
        /// <summary>
        /// Récupère les stats en format texte (debug)
        /// </summary>
        public string GetKeyStatsText()
        {
            _port.DiscardInBuffer();
            _port.WriteLine("KEYSTATS?");
            
            StringBuilder sb = new StringBuilder();
            string line;
            
            // Lire jusqu'à "OK"
            while ((line = _port.ReadLine()) != null)
            {
                sb.AppendLine(line.Trim());
                if (line.Trim() == "OK") break;
            }
            
            return sb.ToString();
        }
        
        /// <summary>
        /// Remet les stats à zéro
        /// </summary>
        public bool ResetKeyStats()
        {
            _port.DiscardInBuffer();
            _port.WriteLine("KEYSTATS_RESET");
            
            string response = _port.ReadLine();
            return response.Contains("OK");
        }
    }
}
```

## Exemple d'utilisation

```csharp
using System;

class Program
{
    static void Main()
    {
        var reader = new KeyStatsReader("COM3"); // Adapter le port
        
        try
        {
            reader.Open();
            
            // Récupérer les stats
            var stats = reader.GetKeyStats();
            
            Console.WriteLine($"Total touches: {stats.TotalPresses}");
            Console.WriteLine($"Max sur une touche: {stats.MaxPresses}");
            Console.WriteLine();
            
            // Afficher la heatmap en console
            for (int r = 0; r < stats.Rows; r++)
            {
                for (int c = 0; c < stats.Cols; c++)
                {
                    double intensity = stats.GetIntensity(r, c);
                    
                    // Caractère selon intensité
                    char ch = intensity switch
                    {
                        < 0.1 => '·',
                        < 0.3 => '░',
                        < 0.5 => '▒',
                        < 0.7 => '▓',
                        _ => '█'
                    };
                    
                    Console.Write($"{ch} ");
                }
                Console.WriteLine();
            }
            
            // Ou afficher les valeurs brutes
            Console.WriteLine("\nValeurs brutes:");
            for (int r = 0; r < stats.Rows; r++)
            {
                Console.Write($"R{r}: ");
                for (int c = 0; c < stats.Cols; c++)
                {
                    Console.Write($"{stats.PressCount[r, c],5} ");
                }
                Console.WriteLine();
            }
        }
        finally
        {
            reader.Close();
        }
    }
}
```

## Intégration WPF/WinForms pour heatmap visuelle

```csharp
// Dans un contrôle WPF ou WinForms, dessiner chaque touche:

void DrawHeatmap(Graphics g, KeyStats stats, Rectangle[,] keyRects)
{
    for (int r = 0; r < stats.Rows; r++)
    {
        for (int c = 0; c < stats.Cols; c++)
        {
            var (red, green, blue) = stats.GetHeatmapColor(r, c);
            
            using (var brush = new SolidBrush(Color.FromArgb(red, green, blue)))
            {
                g.FillRectangle(brush, keyRects[r, c]);
            }
            
            // Optionnel: afficher le nombre
            string count = stats.PressCount[r, c].ToString();
            g.DrawString(count, Font, Brushes.White, keyRects[r, c], 
                new StringFormat { Alignment = StringAlignment.Center });
        }
    }
}
```

## Notes importantes

1. **Positions VERSION_2**: Les données sont déjà traduites en positions VERSION_2 par le firmware, donc elles correspondent directement à ton layout PC.

2. **Persistance**: Les stats ne sont **pas sauvegardées en NVS** actuellement - elles sont perdues au reboot. Si tu veux les persister, il faudra ajouter une fonction de sauvegarde.

3. **Thread-safety**: Les stats sont incrémentées depuis le callback clavier qui tourne sur un autre core. Pour du polling fréquent, c'est OK, mais pour une lecture atomique parfaite il faudrait un mutex.

4. **Refresh recommandé**: Toutes les 1-5 secondes pour une heatmap temps réel.
