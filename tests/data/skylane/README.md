# SkyLane geometry reference

`dragon_changes.txt` contains tower visit counts per model layer, extracted from
the user-supplied `DRAGON_D2_skylane_Rectangle.gcode` in
`SKYLANE_SPEED_TEST_2026-10-06 2 (2).zip`. D4 (Cone) and D6 (Rib) use the same
sequence. There are 272 model layers and 395 visits; the final 13 layers have no
tower visits. The first layer is 0.25 mm and subsequent layers are 0.22 mm.

Reference settings: width 28 mm, prime volume 38 mm³, ramming 5 mm³,
purge spacing 120%, maximum dip 0.4 mm, nozzle diameter 0.4 mm, line width
0.5 mm, brim 5 mm, cone angle 15°, rib length 8 mm, rib width 8 mm, fillet on.

The rectangular packer's expected depth in `test_skylane.cpp` comes from the
supplied G-code. The emitter intentionally uses a square body following the
subsequent user request, with corner connections instead of disconnected fill
beside the ribs. Its tests assert those properties independently of the original
non-square dimensions. The original G-code is not needed to run tests.
