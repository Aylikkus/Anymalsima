# Sky textures

The sky runs on the saved world clock. One game day takes 72 real minutes at
normal speed. New worlds begin at 08:00. Use the time speed button to preview
sunrise, sunset, and night faster.

The shader draws the daytime gradient, stars and galaxy. Each world seed
determines where the galaxy appears and how its stars and dust are arranged.
The pattern stays fixed as the sun and moon move across it.

The sun and moon come from `assets/textures/sky/sun.png` and
`assets/textures/sky/moon.png`. Use square PNGs with straight alpha, a
centered disc, and fully transparent edges. In the supplied sprites, the
sun disc covers 30% of the image width and the moon disc covers 24.5%.
The rest of the canvas gives any glow room to fade.

Run `python3 tools/generate_sky_textures.py` to regenerate both sprites
with NumPy and Pillow. The script replaces the supplied PNGs. Use
`--output-dir` to write them elsewhere. Run `python3 generate_assets.py`
to refresh the asset manifest and compiled shaders, then rebuild `app`
to refresh its copied assets.
