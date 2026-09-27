# Diablo II: Resurrected in 3D

Adds camera control to Diablo II: Resurrected.
Press F12 to activate, mouse wheel to zoom, middle mouse down to drag the camera.

![](img/diablo.jpg)
![](img/bloodraven.jpg)
![](img/pindleskin.jpg)

## Standalone version

**This will trigger Diablo II's integrity check and the game will crash after a minute.**

```
bazel build //standalone:dist
```

Copy `bazel-bin/standalone/dist/d2r-3d.{exe.dll}` directly into your D2R folder (next to D2R.exe) and run `d2r-3d.exe`.

Press F12 to activate in the game.

## Plugin version

`bazel build //standalone:dist`. No further documentation, sorry.
