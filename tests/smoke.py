#!/usr/bin/env python3
"""Lux-Local smoke test: builds nothing, runs build/app in a sandbox (own X server, own Luxium host, temp HOME/XDG).

  tests/smoke.py [--app build/app] [--luxium ../Luxium/build/luxium] [--display :N] [scenario ...]
Scenarios: luxium webkit nobin  (default: all)
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smokelib as L

ap = argparse.ArgumentParser()
ap.add_argument("--app", default=os.path.join(os.path.dirname(__file__), "..", "build", "app"))
ap.add_argument("--luxium", default=L.LUXIUM_DEFAULT)
ap.add_argument("--display")
ap.add_argument("only", nargs="*")
a = ap.parse_args()
APP = os.path.realpath(a.app)
ROUTES = [("/", None, 200), ("/api/settings", None, 200), ("/api/debug/simple", None, 200), ("/nope-not-a-route", None, 404)]

xs = L.XServer(a.display)
print("display", xs.display)
try:
    def want(n): return not a.only or n in a.only
    if want("luxium"): L.scenario_luxium("localify", APP, [], a.luxium, xs.display, ROUTES)
    if want("webkit"): L.scenario_webkit("localify", APP, [], xs.display, ROUTES, no_binary=False)
    if want("nobin"): L.scenario_webkit("localify", APP, [], xs.display, ROUTES, no_binary=True)
finally:
    xs.stop()
sys.exit(L.summary())
