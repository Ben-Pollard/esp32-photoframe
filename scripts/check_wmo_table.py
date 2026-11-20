#!/usr/bin/env python3
"""Verify main/weather.c's WMO table is a verbatim copy of the Open-Meteo table.

The table exists only because the API returns a bare number and no description, so
its whole value is being faithful to the published mapping. Paraphrasing is how
the last version drifted -- it also omitted code 97 entirely, which made a heavy
thunderstorm expand to "unremarkable weather". This makes that impossible to miss.

    ./check_wmo_table.py            exit 0 clean, exit 1 with a report of drift
"""
import json
import os
import re
import sys

LAB = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(LAB, "..", "main", "weather.c")
JSON = os.path.join(LAB, "..", "docs", "wmo_codes_openmeteo.json")

docs = {k: v for k, v in json.load(open(JSON)).items() if not k.startswith("_")}
src = open(SRC).read()

start = src.index("} WMO[] = {")
end = src.index("const char *weather_wmo_description")
ours = {c: t for c, t in re.findall(r'\{(\d+),\s*"([^"]*)"\}', src[start:end])}

problems = []

missing = sorted(set(docs) - set(ours), key=int)
if missing:
    problems.append("Codes Open-Meteo defines that weather.c does NOT have:")
    problems += [f"  {c:>3}  {docs[c]}" for c in missing]

extra = sorted(set(ours) - set(docs), key=int)
if extra:
    problems.append("Codes in weather.c that Open-Meteo does NOT define:")
    problems += [f"  {c:>3}  {ours[c]}" for c in extra]

drift = [(c, ours[c], docs[c]) for c in sorted(set(ours) & set(docs), key=int)
         if ours[c] != docs[c]]
if drift:
    problems.append("Text is not verbatim (ours / docs):")
    for c, o, dd in drift:
        problems.append(f"  {c:>3}  {o!r}")
        problems.append(f"       {dd!r}")

if "return NULL;" not in src[start:src.index("const char *weather_wmo_description") + 400] \
        and "return NULL;" not in src[end:end + 400]:
    problems.append("weather_wmo_description must return NULL for an unknown code. "
                    "A invented fallback description is what mis-reported code 97.")

if problems:
    print("\n".join(problems))
    print(f"\nFAIL  docs: {len(docs)} codes, weather.c: {len(ours)} codes")
    sys.exit(1)

print(f"OK  weather.c carries all {len(docs)} Open-Meteo codes verbatim")
