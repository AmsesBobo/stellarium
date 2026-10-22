#!/usr/bin/env python3
"""
Make textures/milkyway_gaia_correction.png, the large-scale correction of the photographic Milky Way
(see MilkyWay::init() in src/core/modules/MilkyWay.cpp).

The Gaia Sky image (textures/milkyway_gaia.png), expanded by the photographic curve of MilkyWay.cpp, is compared
with Axel Mellinger's photometric panorama (the 3600 x 1800 FITS version, galactic plate carree, 0.1 deg per pixel,
Mellinger 2009, PASP 121, 1180; http://galaxy.phy.cmich.edu/~axel/mwpan2/mwpan2_RGB_3600.fits, offered for science
use, all rights reserved - the correction derived from it is not for redistribution without his consent):

1. The flux scale of the FITS file from the stars: each of its pixels is a single (bilinear) sample of the full
   panorama, so single stars are not measured right, but the mean over many is. About 1400 isolated catalog stars
   of V 3.5-7.5 (|b| > 25) give the zero point Q (green channel ~ V): mu_V = Q + 12.78 - 2.5 lg(value).
2. Both maps on a galactic grid of 0.5 deg (mean of the FITS samples, stars brighter than 7.5 masked; mean of the
   expanded Gaia texture in cd/m^2).
3. The smooth surplus d = S(Gaia) - S(Mellinger), S a Gaussian smoothing of 3 deg, in units of the expanded
   texture, faded out between |b| = 25 and 40. MilkyWay.cpp takes a part of it (astro/milky_way_photo_correction_
   strength, 0.5 by default) off the luminance of the texture.
4. Stored in Stellarium's equatorial layout (1024 x 512, column 0.25 - RA/360, top row Dec +90) as
   0.5 + 0.5 sign(d) sqrt(|d|/scale), with the scale and the curve parameters in PNG text chunks (read by
   MilkyWay.cpp: the correction is switched off if the curve there differs).

Usage: make_milkyway_correction.py --fits mwpan2_RGB_3600.fits --stellarium <source tree> [--out file.png]
Needs numpy, scipy, astropy, Pillow.
"""
import argparse, os, struct
import numpy as np
from scipy import ndimage
from PIL import Image, PngImagePlugin

# The expansion of the Gaia image in MilkyWay::init() (photoCurve, photoCurveTop) and its luminance scale:
# keep them equal to the code, the texture records them
CURVE = (-0.48191, 4.11797, 1.14056, -1.50713)
CURVE_TOP = -0.8882
LUMINANCE = 5.096e-3            # cd/m^2 of an expanded texture value of 1.0
GAL = np.array([[-0.0548755604, -0.8734370902, -0.4838350155],   # J2000 -> galactic
                [0.4941094279, -0.4448296300, 0.7469822445],
                [-0.8676661490, -0.1980763734, 0.4559837762]])
CELL = 0.5

def srgb_to_linear(v):
    return np.where(v <= 0.04045, v/12.92, ((v + 0.055)/1.055)**2.4)

def expand(L):
    c0, c1, c2, x0 = CURVE
    x = np.log(np.maximum(L, 1e-6)); xe = np.clip(x, x0, CURVE_TOP)
    return np.exp(c0 + xe*(c1 + xe*c2) + (c1 + 2.*c2*xe)*(x - xe)) * (L > 0)

def read_stars(tree):
    """Stellarium's binary star catalogs of levels 0 and 1 (Hipparcos/Gaia, to V 7.5): unit vectors, V, B-V"""
    dt = np.dtype([('gaia', '<i8'), ('x0', '<i4'), ('x1', '<i4'), ('x2', '<i4'), ('dx0', '<i4'), ('dx1', '<i4'),
                   ('dx2', '<i4'), ('bv', '<i2'), ('v', '<i2'), ('plx', '<u2'), ('plxe', '<u2'), ('rv', '<i2'),
                   ('sp', '<u2'), ('ot', 'u1'), ('hip', 'u1', 3)])
    out = []
    folder = os.path.join(tree, 'stars', 'hip_gaia3')
    for fn in sorted(os.listdir(folder)):
        if not (fn.startswith('stars_0_') or fn.startswith('stars_1_')) or not fn.endswith('.cat'):
            continue
        b = open(os.path.join(folder, fn), 'rb').read()
        magic, typ, major, minor, level, magmin, epoch = struct.unpack('<IiiiiiI', b[:28])
        nz = 20*4**level + 1
        sizes = np.frombuffer(b[28:28 + 4*nz], dtype='<u4'); off = 28 + 4*nz
        n = int(sizes.astype(np.int64).sum())
        r = np.frombuffer(b[off:off + 48*n], dtype=dt)
        x = np.vstack([r['x0'], r['x1'], r['x2']]).T/2e9
        out.append(np.column_stack([x, r['v']/1000., r['bv']/1000.]))
    return np.vstack(out)

def fits_pixel(l, b):
    """pixel (0-based) of the 3600 x 1800 FITS for galactic l, b (CRPIX 1800.95/900.95, l growing to the left)"""
    return (1800.95 - 1 - (((l + 180.) % 360.) - 180.)/0.1), (900.95 - 1 + b/0.1)

def star_zero_point(G, stars):
    g = stars[:, :3] @ GAL.T
    l = np.degrees(np.arctan2(g[:, 1], g[:, 0])) % 360; b = np.degrees(np.arcsin(np.clip(g[:, 2], -1, 1)))
    X, Y = fits_pixel(l, b); V = stars[:, 3]
    from scipy.spatial import cKDTree
    tree = cKDTree(stars[:, :3])
    vals = []
    for i in np.where((V > 6.0) & (V < 7.5) & (np.abs(b) > 25))[0]:
        if any(V[j] < V[i] + 2.5 for j in tree.query_ball_point(stars[i, :3], np.radians(1.2)) if j != i):
            continue
        xi, yi = int(round(X[i])), int(round(Y[i]))
        if yi < 12 or yi > 1786:
            continue
        yy, xx = np.mgrid[-11:12, -11:12]; rr = np.hypot(xx + xi - X[i], yy + yi - Y[i])
        cut = G[yi-11:yi+12, :].take(range(xi-11, xi+12), axis=1, mode='wrap')
        f = (cut[rr <= 3] - np.median(cut[(rr >= 6) & (rr <= 11)])).sum()
        vals.append(f*10**(0.4*(V[i] - 0.02*stars[i, 4])))
    return 2.5*np.log10(np.mean(vals)), len(vals)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fits', required=True); ap.add_argument('--stellarium', required=True)
    ap.add_argument('--out', default='milkyway_gaia_correction.png'); ap.add_argument('--zero-point', type=float)
    a = ap.parse_args()
    from astropy.io import fits
    G = fits.getdata(a.fits)[1].astype(np.float64)        # planes B, G, R: green ~ V
    stars = read_stars(a.stellarium)
    if a.zero_point is None:
        Q, n = star_zero_point(G, stars)
        print('zero point of the FITS (green): %.3f from %d stars' % (Q, n))
    else:
        Q = a.zero_point
    nl, nb = int(360/CELL), int(180/CELL)
    # Mellinger: mean of the samples per cell, stars brighter than 7.5 masked
    g = stars[:, :3] @ GAL.T
    sl = np.degrees(np.arctan2(g[:, 1], g[:, 0])) % 360; sb = np.degrees(np.arcsin(np.clip(g[:, 2], -1, 1)))
    X, Y = fits_pixel(sl, sb); mask = np.zeros(G.shape, bool)
    for x, y, v in zip(np.round(X).astype(int) % 3600, np.round(Y).astype(int), stars[:, 3]):
        r = 1 if v > 4.5 else (2 if v > 2.5 else 4)
        mask[max(y-r, 0):min(y+r+1, 1800)][:, np.arange(x-r, x+r+1) % 3600] = True
    L = (-(np.arange(3600) - (1800.95 - 1))*0.1) % 360; B = (np.arange(1800) - (900.95 - 1))*0.1
    LI, BI = np.meshgrid((L/CELL).astype(int) % nl, np.clip(((B + 90)/CELL).astype(int), 0, nb-1))
    w = (~mask).astype(float); idx = (BI*nl + LI).ravel()
    sw = np.bincount(idx, w.ravel(), nl*nb)
    MG = (np.bincount(idx, (G*w).ravel(), nl*nb)/np.maximum(sw, 1)).reshape(nb, nl); cnt = sw.reshape(nb, nl)
    muM = Q + 12.78 - 2.5*np.log10(np.maximum(MG, 1e-3))
    # Gaia: the expanded texture per cell (cos(dec) weighted)
    tex = srgb_to_linear(np.asarray(Image.open(os.path.join(a.stellarium, 'textures', 'milkyway_gaia.png')).convert('RGB')).astype(np.float32)/255)
    E = expand(tex @ np.array([0.2126, 0.7152, 0.0722], np.float32))
    H, W = E.shape; rr, cc = np.mgrid[0:H, 0:W]
    dec = np.radians(90 - (rr + 0.5)/H*180); ra = np.radians(90 - (cc + 0.5)/W*360)
    v = np.stack([np.cos(dec)*np.cos(ra), np.cos(dec)*np.sin(ra), np.sin(dec)], -1) @ GAL.T
    gl = np.degrees(np.arctan2(v[..., 1], v[..., 0])) % 360; gb = np.degrees(np.arcsin(np.clip(v[..., 2], -1, 1)))
    gidx = (np.clip(((gb + 90)/CELL).astype(int), 0, nb-1)*nl + (gl/CELL).astype(int) % nl).ravel()
    wt = np.cos(dec).ravel(); gw = np.bincount(gidx, wt, nl*nb)
    GE = (np.bincount(gidx, E.ravel()*wt, nl*nb)/np.maximum(gw, 1e-9)).reshape(nb, nl)
    muG = -2.5*np.log10(np.maximum(GE*LUMINANCE, 1e-12)/10.8e4)
    ok = (cnt > 8) & (MG > 0) & (gw.reshape(nb, nl) > 0) & (muG < 29) & (muM < 29)
    # the smooth surplus, in units of the expanded texture
    toE = 10.8e4/LUMINANCE
    EG = np.where(ok, 10**(-0.4*muG)*toE, 0.); EM = np.where(ok, 10**(-0.4*muM)*toE, 0.); Wk = ok.astype(float)
    sm = lambda A: ndimage.gaussian_filter(A, 3./CELL, mode=('nearest', 'wrap'))/np.maximum(ndimage.gaussian_filter(Wk, 3./CELL, mode=('nearest', 'wrap')), 1e-6)
    CB = (np.arange(nb) + 0.5)*CELL - 90
    P = (sm(EG) - sm(EM))*np.clip((40 - np.abs(CB))/15, 0, 1)[:, None]
    scale = float(np.percentile(np.abs(P), 99.95))
    # to the equatorial layout of the textures
    Ho, Wo = 512, 1024; rr, cc = np.mgrid[0:Ho, 0:Wo]
    dec = np.radians(90 - (rr + 0.5)/Ho*180); ra = np.radians(90 - (cc + 0.5)/Wo*360)
    v = np.stack([np.cos(dec)*np.cos(ra), np.cos(dec)*np.sin(ra), np.sin(dec)], -1) @ GAL.T
    gl = np.degrees(np.arctan2(v[..., 1], v[..., 0])) % 360; gb = np.degrees(np.arcsin(np.clip(v[..., 2], -1, 1)))
    yi = (gb + 90)/CELL - 0.5; xi = gl/CELL - 0.5; xi = np.where(xi < 0, xi + nl, xi)
    Pe = ndimage.map_coordinates(np.concatenate([P, P[:, :2]], axis=1), [np.clip(yi, 0, nb-1), xi], order=1, mode='nearest')
    img = np.round((0.5 + 0.5*np.sign(Pe)*np.sqrt(np.minimum(np.abs(Pe)/scale, 1)))*255).astype(np.uint8)
    info = PngImagePlugin.PngInfo()
    info.add_text('surplus_scale', '%.5f' % scale)
    info.add_text('photo_curve', ','.join('%.5f' % c for c in CURVE + (CURVE_TOP,)))
    info.add_text('Source', 'Gaia Sky Milky Way image vs. A. Mellinger, PASP 121, 1180 (2009); see util/milkyway_correction')
    Image.fromarray(np.stack([img]*3, -1)).save(a.out, pnginfo=info)
    print('written %s: surplus scale %.5f (expanded texture units), zero point %.3f' % (a.out, scale, Q))

if __name__ == '__main__':
    main()
