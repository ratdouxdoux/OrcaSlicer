# Full Spectrum Color Reference

Use this catalog to choose a color visually and copy its filament recipe into Snorca.

Open the standalone catalog here:

- [Full Spectrum Color Reference HTML](full-spectrum-color-reference.html)

> **Choose the swatch by appearance, then use its recipe.** The tile is the visual reference; copy only its F1-F4 labels and percentages into Color Mixing.

## Quick Start

1. Add four physical filaments in the Prepare sidebar.
2. Set the slot colors in this order: **F1 Cyan, F2 Magenta, F3 Yellow, F4 Grey**.
3. Open the HTML catalog and choose the tile that looks closest to the color you want.
4. Read the F-slots and percentages on that tile.
5. Click the **+** button beside **Color Mixing** and enter the listed recipe.
6. Click **OK**, assign the new color mix to the model or painted region, and slice.
7. Make a small test print when color is important.

## 1. Set the Filament Colors

The reference uses the following slot order:

| Slot | Color |
| --- | --- |
| F1 | Cyan |
| F2 | Magenta |
| F3 | Yellow |
| F4 | Grey |

In the Prepare sidebar:

1. Add the four physical filaments under **Filaments**.
2. Click the color chip beside each filament and select its color.
3. Check the order carefully. The same percentages assigned to different F-numbers will produce a different result.

The **Color Mixing** section appears after at least two physical filaments are configured.

## 2. Choose and Read a Swatch

The catalog contains two groups:

- **Reference Color Recipes** are the main set of colors.
- **Additional Color Recipes** provide more choices and should be confirmed with a small test print.

Each tile shows:

| Tile field | Meaning |
| --- | --- |
| Number | A convenient reference number for finding the tile again. |
| Tile color | The expected visual appearance to compare on screen. |
| Slots, such as `F1/F2` | The physical filaments used by the recipe. |
| Recipe, such as `F1 50% / F2 50%` | The shares to enter in Color Mixing. |

Use the browser's Find command (`Ctrl+F` on Windows) to locate an F-slot combination or percentage recipe.

## 3. Create the Recipe

Click the **+** button beside **Color Mixing** to open **Add Mix**.

### Two-Filament Colors

1. Select **Ratio**.
2. Choose the two F-slots listed on the tile.
3. Move the **Mixing Ratio** control to the listed percentages, or the closest whole-percent values.
4. Click **OK**.

Example: tile `5` uses `F1 50% / F2 50%`.

### Three-Filament Colors

1. Select **Ratio**.
2. Add a third filament row under **Filament Selection**.
3. Choose the three F-slots listed on the tile.
4. Move the triangular ratio selector until the percentage labels are as close as possible to the tile.
5. Click **OK**.

Example: tile `83` uses equal shares of F1, F2, and F3.

### Four-Filament Colors

1. Select **Cycle**.
2. Use the numbered filament controls to build a repeating pattern with the shares shown on the tile.
3. Check the percentage summary and adjust the pattern until it matches the recipe.
4. Click **OK**.

For example, `1234` gives each filament an equal share, while `12344` gives F1, F2, and F3 20% each and F4 40%.

Different Cycle orders with the same percentages can look different on a printed part. Keep an existing order when editing a saved project, and make a small test print when creating a new one.

## 4. Apply and Check the Color

After clicking **OK**, the new color mix appears under **Color Mixing** and can be assigned to an object, part, or painted region.

Before a production print:

1. Confirm the Color Mixing summary uses the intended F-slots and percentages.
2. Check that the new color mix is assigned to the intended geometry.
3. Slice the model and use the preview to confirm the assignment.
4. Print a small sample and compare it under the lighting where the finished part will be used.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| **Color Mixing** is missing | Configure at least two physical filaments. |
| The color is very different from the tile | Recheck the F1-F4 color order and recipe percentages. |
| A fractional percentage cannot be selected | Use the closest whole-percent setting and make a test print. |
| A four-color recipe cannot be entered in Ratio | Use Cycle mode and match the percentage summary. |
| The print differs from the screen | Filament, print settings, part geometry, lighting, and the display can all affect appearance. |

## Important Limits

- The catalog is a visual planning reference, not a guaranteed color match.
- Different filament brands, colors, and spool lots can produce different results.
- Layer height, wall thickness, surface direction, part geometry, and printer setup can change the visible color.
- Monitor settings and room lighting affect how the catalog appears on screen.
- Always make a small test print when the final color matters.
