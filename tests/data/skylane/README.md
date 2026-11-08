# SkyLane geometry reference

`dragon_changes.txt` contains tower visit counts per model layer, extracted from
the user-supplied `DRAGON_D2_skylane_Rectangle.gcode` in
`SKYLANE_SPEED_TEST_2026-10-06 2 (2).zip`. D4 (Cone) and D6 (Rib) use the same
sequence. There are 272 model layers and 395 visits; the final 13 layers have no
tower visits. The first layer is 0.25 mm and subsequent layers are 0.22 mm.

Reference settings: width 28 mm, prime volume 38 mm³, ramming 5 mm³,
purge spacing 120%, maximum dip 0.4 mm, nozzle diameter 0.4 mm, line width
0.5 mm, brim 5 mm, cone angle 15°, rib length 8 mm, rib width 8 mm, fillet on.

The expected dimensions in `test_skylane.cpp` come from the supplied G-code,
not from this implementation. The original G-code is not needed to run tests.


`colour_visits.txt` records the 510 tower tool changes from the user-supplied
`random-Cube_PLA_2h16m.gcode`: nozzle Z in mm, outgoing tool, incoming tool.
The first tower visit is structural at 0.20 mm; subsequent model layers are
0.08 mm. Tool colours are cyan (0), magenta (1), black (2), and yellow (3).
Reference settings: width 30 mm, prime volume 18 mm³, outgoing ramming 5 mm³,
120% purge spacing, 0.4 mm dip and nozzle, 0.5 mm line width, rib wall.
The stock tool-change paths span 32 mm in X, giving 31 mm merged purge rows;
the replay preserves these path bounds separately from the 30 mm packing width.
This fixture reproduces small outgoing-colour islands and a colour returning
to the same physical plane. Tests also replay it with rectangle and cone walls.
