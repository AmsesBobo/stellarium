#!/usr/bin/env python3
# Measure the images of emission nebulae against the all-sky H-alpha map of Finkbeiner (2003, ApJS 146, 407)
# and write "halphaScale" into nebulae/default/textures.json (see README.txt).
#
# Usage: halpha_calibration.py Halpha_fwhm06_1024.fits [--write]
#
# For each image listed in halpha_images.txt: the texture as the photographic mode draws it (linear, to the power
# PHOTO_DSO_DESTRETCH = 1.5, luminance Y), smoothed to the 6 arcmin beam of the map, is sampled at the map pixels
# inside the image. halphaScale = median over the bright parts of the nebula (H-alpha above the local background
# more than max(5 R, 30% of its 95th percentile)) of (H-alpha - background) / texture. The background is the 5th
# percentile of the map in the image. Stellarium multiplies it by PHOTO_HALPHA_LUMINANCE (cd/m^2 per Rayleigh).
# Requirements: numpy, scipy, healpy, Pillow.
import json, math, re, sys, os
import numpy as np, healpy as hp
from PIL import Image
from scipy.ndimage import gaussian_filter, map_coordinates

HERE = os.path.dirname(os.path.abspath(__file__))
NEB = os.path.join(HERE, '..', '..', 'nebulae', 'default')
DESTRETCH = 1.5
BEAM = 6./60.  # degrees (FWHM)


def vec(ra, de):
    ra, de = math.radians(ra), math.radians(de)
    return np.array([math.cos(de)*math.cos(ra), math.cos(de)*math.sin(ra), math.sin(de)])


def srgb_to_linear(x):
    return np.where(x <= 0.04045, x/12.92, ((x+0.055)/1.055)**2.4)


def gnomonic(v, c):
    east = np.cross([0., 0., 1.], c)
    east /= np.linalg.norm(east)
    north = np.cross(c, east)
    w = v @ c
    return np.stack([(v @ east)/w, (v @ north)/w], axis=-1)


def inverse_bilinear(p, q):
    # texture coordinates (u, v) of the points p in the quadrilateral q (corners for (0,0), (1,0), (1,1), (0,1))
    u = np.full(len(p), 0.5)
    v = np.full(len(p), 0.5)
    for _ in range(20):
        P = ((1-u)*(1-v))[:, None]*q[0] + (u*(1-v))[:, None]*q[1] + (u*v)[:, None]*q[2] + ((1-u)*v)[:, None]*q[3]
        Pu = (1-v)[:, None]*(q[1]-q[0]) + v[:, None]*(q[2]-q[3])
        Pv = (1-u)[:, None]*(q[3]-q[0]) + u[:, None]*(q[2]-q[1])
        r = P - p
        det = Pu[:, 0]*Pv[:, 1] - Pu[:, 1]*Pv[:, 0]
        u -= (r[:, 0]*Pv[:, 1] - r[:, 1]*Pv[:, 0])/det
        v -= (Pu[:, 0]*r[:, 1] - Pu[:, 1]*r[:, 0])/det
    return u, v


def measure(tile, hamap, nside, rot):
    corners = [vec(*p) for p in tile['worldCoords'][0]]
    c = sum(corners)
    c /= np.linalg.norm(c)
    radius = max(math.degrees(math.acos(min(1., x @ c))) for x in corners)
    a = srgb_to_linear(np.asarray(Image.open(os.path.join(NEB, tile['imageUrl'])).convert('RGB'), dtype=np.float64)/255.)
    Y = (a**DESTRETCH) @ np.array([0.2126, 0.7152, 0.0722])
    h, w = Y.shape
    ipix = hp.query_polygon(nside, np.array(rot(np.array(corners).T)).T, inclusive=False)  # the map is galactic
    pv = np.array(rot.I(np.array(hp.pix2vec(nside, ipix)))).T
    u, v = inverse_bilinear(gnomonic(pv, c), gnomonic(np.array(corners), c))
    inside = (u >= 0) & (u <= 1) & (v >= 0) & (v <= 1)
    degPerTexel = 2*radius/math.sqrt(2)/max(w, h)
    Ys = gaussian_filter(Y, sigma=max(BEAM/2.355/degPerTexel, 0.5))
    # texture coordinate t = 0 is the bottom row of the picture
    T = map_coordinates(Ys, [(1-v[inside])*(h-1), u[inside]*(w-1)], order=1)
    Ha = hamap[ipix[inside]]
    good = np.isfinite(Ha) & (Ha > -100)
    T, Ha = T[good], Ha[good]
    excess = Ha - np.percentile(Ha, 5)
    peak = np.percentile(excess, 95)
    bright = (excess > max(5., 0.3*peak)) & (T > 0)
    scale = float(np.median(excess[bright]/T[bright])) if bright.sum() >= 10 else float('nan')
    corr = float(np.corrcoef(T, excess)[0, 1])
    return scale, corr, float(peak), int(len(T))


def main():
    hamap = hp.read_map(sys.argv[1])
    nside = hp.get_nside(hamap)
    rot = hp.Rotator(coord=['C', 'G'])
    names = [l.strip() for l in open(os.path.join(HERE, 'halpha_images.txt')) if l.strip() and not l.startswith('#')]
    path = os.path.join(NEB, 'textures.json')
    raw = open(path, 'rb').read()
    tiles = {t['imageUrl']: t for t in json.loads(raw.decode('utf-8'), strict=False)['subTiles']}
    results = {}
    for name in names:
        scale, corr, peak, n = measure(tiles[name], hamap, nside, rot)
        results[name] = scale
        print('%-28s halphaScale %10.4g   correlation %5.2f   peak %7.1f R   %d map pixels' % (name, scale, corr, peak, n))
    if '--write' in sys.argv:
        raw = re.sub(rb'\n[ \t]*"halphaScale"\s*:\s*[-0-9.eE+]+,\r?\n', b'\n', raw)  # remove old values
        for name, scale in results.items():
            pat = re.compile(rb'(\n([ \t]*)"imageUrl"\s*:\s*"' + re.escape(name.encode()) + rb'",(\r?\n))')
            raw = pat.sub(lambda m: m.group(1) + m.group(2) + b'"halphaScale" : ' + ('%.4g' % scale).encode() + b',' + m.group(3), raw, count=1)
        open(path, 'wb').write(raw)
        print('written to', path)


if __name__ == '__main__':
    main()
