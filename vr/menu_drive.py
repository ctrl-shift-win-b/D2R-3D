"""Pointer helper for the D2R window.

Clicks only the coordinates given on the command line, after a person has
looked at a screenshot. It has no keyboard input and no delete path.
"""

import sys

import pyautogui

pyautogui.FAILSAFE = True
pyautogui.PAUSE = 0.12


def shot(path):
    image = pyautogui.screenshot()
    image.save(path)
    print(f"shot {image.size[0]}x{image.size[1]} {path}")


def main():
    if len(sys.argv) < 2:
        print("usage: menu_drive.py shot|move|click|windows|pos ...")
        return 2
    cmd = sys.argv[1]
    if cmd == "shot":
        shot(sys.argv[2])
    elif cmd == "move":
        x, y = int(sys.argv[2]), int(sys.argv[3])
        pyautogui.moveTo(x, y, duration=0.15)
        print(f"move {x} {y}")
    elif cmd == "click":
        x, y = int(sys.argv[2]), int(sys.argv[3])
        pyautogui.click(x, y)
        print(f"click {x} {y}")
    elif cmd == "windows":
        import pygetwindow as gw

        for win in gw.getAllWindows():
            if win.title.strip():
                print(f"{win.left},{win.top},{win.width}x{win.height} {win.title}")
    elif cmd == "pos":
        print(pyautogui.position())
    else:
        print("unknown command")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
