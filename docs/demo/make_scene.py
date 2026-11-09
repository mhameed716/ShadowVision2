"""Make a scene anyone can judge by eye, plus the mask a user would paint.

Sky gradient, sun, two mountain ridges, textured ground, a red post (the
"intruder" a user removes) and a thin cable across the sky (the hard case: a
2-3 px structure that naive inpainting smears into a grey band).
"""
import numpy as np

W, H = 900, 600
yy, xx = np.mgrid[0:H, 0:W]
img = np.zeros((H, W, 3), np.float32)

# sky
t = (np.arange(H, dtype=np.float32) / H)[:, None]
img[:, :, 0] = 90 + 110 * t
img[:, :, 1] = 140 + 80 * t
img[:, :, 2] = 200 + 40 * t

# sun with a soft halo
d = np.sqrt((xx - 720.0) ** 2 + (yy - 130.0) ** 2)
img[d < 70] = [255, 240, 190]
halo = (d >= 70) & (d < 95)
w = ((95 - d[halo]) / 25.0)[:, None]
img[halo] = img[halo] * (1 - w) + np.array([255, 235, 180]) * w


def ridge(base, amp, freq, phase, color):
    for x in range(W):
        top = int(base - amp * np.sin(x * freq + phase)
                  - amp * 0.4 * np.sin(x * freq * 2.3 + 1.7))
        img[max(0, top):, x] = color


ridge(430, 60, 0.012, 0.0, [70, 85, 95])
ridge(470, 40, 0.020, 2.1, [52, 66, 74])

# ground with real high-frequency texture (so resizing has something to fight)
img[500:, :] = [58, 52, 46]
rng = np.random.default_rng(7)
img[500:, :] += (rng.random((H - 500, W, 1)).astype(np.float32) * 26 - 13)

# the intruder: a red post
img[250:505, 160:200] = [190, 60, 55]
img[250:262, 150:210] = [210, 80, 70]

# the hard case: a thin cable across the sky
for x in range(60, 860):
    y = int(300 + 25 * np.sin(x / 90.0))
    img[y:y + 3, x] = [30, 30, 34]

img = np.clip(img, 0, 255).astype(np.uint8)
with open('/tmp/demo/scene.ppm', 'wb') as f:
    f.write(b'P6\n%d %d\n255\n' % (W, H))
    f.write(img.tobytes())

mask = np.zeros((H, W), np.uint8)
mask[244:512, 154:206] = 255
for x in range(55, 865):
    y = int(300 + 25 * np.sin(x / 90.0))
    mask[max(0, y - 3):y + 6, x] = 255
with open('/tmp/demo/mask.pgm', 'wb') as f:
    f.write(b'P5\n%d %d\n255\n' % (W, H))
    f.write(mask.tobytes())

print('scene.ppm + mask.pgm written', img.shape, 'mask px:', int((mask > 0).sum()))
