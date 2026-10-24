H-alpha calibration of the images of emission nebulae (photographic mode)

In the photographic mode, the images of nebulae are drawn in physical units. Most of them are calibrated by the
catalog magnitude of the brightest object in the image (see StelSkyImageTile::computePhotoLuminanceScale()).
Emission nebulae have no useful catalog magnitude: most have none at all (Sharpless, RCW, the California Nebula),
others give the light of the cluster in them (M 16, IC 1396). Their images are measured against the all-sky
H-alpha map of

  D. P. Finkbeiner, "A Full-Sky H-alpha Template for Microwave Foreground Prediction", ApJS 146, 407 (2003)
  (a composite of WHAM, VTSS and SHASSA, 6 arcmin resolution, in Rayleigh)

  https://lambda.gsfc.nasa.gov/data/foregrounds/fink_halpha/Halpha_fwhm06_1024.fits (HEALPix Nside 1024, 49 MB)

halpha_calibration.py compares each image listed in halpha_images.txt with the map and writes "halphaScale" into
nebulae/default/textures.json: the H-alpha intensity (Rayleigh) of a texel of value 1, as the photographic mode
draws the texture (linear, to the power 1.5). Stellarium turns it into luminance with PHOTO_HALPHA_LUMINANCE
(1.0e-7 cd/m^2 per Rayleigh, from the line spectrum of an H II region through the V band; NGC 7000 and M 42,
measured by their catalog magnitudes, give the same within 8%).

  python3 halpha_calibration.py Halpha_fwhm06_1024.fits           (print the measurements)
  python3 halpha_calibration.py Halpha_fwhm06_1024.fits --write   (also update textures.json)

Which images are in the list: images larger than about 20 arcmin whose texture follows the map (correlation 0.5
or more) with at least 20 R of H-alpha above the background, and whose object is an emission nebula (H II region,
emission nebula, cluster with nebulosity, supernova remnant) or that have no catalog object and are red. Not in
it: planetary nebulae (their V light is mostly [O III], not in proportion to H-alpha), galaxies and the Magellanic
Clouds (the light of their images is mostly stars), reflection nebulae, and images that follow the map poorly
(IC 434: the Horsehead strip next to Alnitak).

Requirements: numpy, scipy, healpy, Pillow.
