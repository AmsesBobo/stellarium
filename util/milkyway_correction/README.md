# Large-scale correction of the photographic Milky Way

`textures/milkyway_gaia_correction.png` corrects the large-scale brightness of the Gaia Sky Milky Way image
(`textures/milkyway_gaia.png`) in the photographic mode. See the comments in `MilkyWay::init()`
(`src/core/modules/MilkyWay.cpp`) for the method and the results.

* `make_milkyway_correction.py` makes the map from Axel Mellinger's photometric panorama (the 3600 x 1800 FITS
  file, http://galaxy.phy.cmich.edu/~axel/mwpan2/mwpan2_RGB_3600.fits) and the Gaia Sky image:
  `python3 make_milkyway_correction.py --fits mwpan2_RGB_3600.fits --stellarium <source tree>`
  It writes the scale of the map and the expansion of the texture it was made for into PNG text chunks;
  `MilkyWay.cpp` reads them and leaves the correction off if the expansion there differs.
  Keep `CURVE`, `CURVE_TOP` and `LUMINANCE` in the script equal to `photoCurve`, `photoCurveTop` and
  `photoLuminanceScale` in `MilkyWay.cpp`.
* `pioneer10_toller1981.py`: the Pioneer 10 background starlight of Toller (1981), tables 35 and 36 of Leinert et
  al. (1998), used to check the shape of both maps (the Mellinger panorama follows it within 0.12 mag rms).

How strongly the correction is applied (`astro/milky_way_photo_correction_strength`, 0.5) comes from two linear
raw pictures of the Sagittarius region (Canon EOS R6 at the Grand Canyon, Sony a7 II at White Sands), calibrated
with their own stars: the Great Sagittarius Star Cloud above the sky at its altitude.

License: Mellinger's panorama is "all rights reserved", the FITS file is offered for science use. The map derived
from it is not for redistribution without his consent.
