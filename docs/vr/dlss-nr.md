# Super High and DLSS-NR

**Super High** is the High quality preset plus NVIDIA's **DLSS-NR**, a neural-rendering (denoise) model that cleans up the
picture before DLSS makes the final eye image. Everything else about the picture is the same as High.

## What you need

DLSS-NR runs from a model file called `nvngx_dlssnr.dll`. That file belongs to NVIDIA and is not part of the public DLSS SDK,
so Bloodborne VR cannot include it, and it does not link to a download for it.

- If you already have the file (for example from another VR tool you installed), press **Find file...** on the Super High
  card (or on the Graphics page) and pick it. The launcher copies it next to the game on your PC. Nothing is sent anywhere.
- If you do not have the file, the Super High card stays greyed out and the game runs **High**, which looks almost the same.
- If NVIDIA ever ships DLSS-NR in its official SDK, we will include it with the mod.

You also need an NVIDIA RTX graphics card (DLSS-NR is part of NVIDIA's DLSS).

## What it costs

On an RTX 4090 in full stereo, Super High runs at about 50-54 fps, which is lower than High. A weaker card will be lower still,
so use High if the picture is not smooth.

## More about DLSS

NVIDIA's own page about DLSS: https://developer.nvidia.com/rtx/dlss
