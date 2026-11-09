#!/usr/bin/env python3
"""A tiny stand-in Jellyfin server for exercising Volcano OS's Jellyfin app without a real one.

  tools/mock-jellyfin.py [--port 18096] [--log reports.log]

Serves just enough of the API (public info, Quick Connect, password sign-in, libraries, resume,
next up, latest, seasons/episodes, images, a direct-play stream) and logs every playback report.
Quick Connect is approved on the second poll. Username "jamie", password "pw".
"""
import argparse, json, os, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

TOKEN = "tok-" + "a1b2c3d4e5f6"
UID = "u1"
IMG = open(os.path.join(os.path.dirname(__file__), "..", "assets", "apps", "jellyfin.png"), "rb").read()
polls = {"n": 0}
lock = threading.Lock()

def item(i, name, typ, **kw):
    d = {"Id": i, "Name": name, "Type": typ, "ProductionYear": kw.pop("year", 2020), "ImageTags": {"Primary": "t1"},
         "BackdropImageTags": ["b1"], "Overview": kw.pop("ov", f"{name} is a made-up title served by the mock server."),
         "RunTimeTicks": kw.pop("mins", 100) * 600000000, "Genres": ["Drama"], "CommunityRating": 7.5, "OfficialRating": "PG",
         "UserData": {"PlaybackPositionTicks": kw.pop("pos", 0), "Played": kw.pop("played", False), "UnplayedItemCount": kw.pop("unplayed", 0)},
         "MediaSources": [{"Id": "ms-" + i}]}
    d["IsFolder"] = typ in ("Series", "Season", "CollectionFolder", "BoxSet")
    d.update(kw)
    return d

MOVIES = [item(f"m{k}", n, "Movie", year=2015 + k, pos=(36000000000 if k == 2 else 0)) for k, n in enumerate(["Ashfall", "Tidewater", "Harbor Light", "Paper Moon", "Nine Lives"], 1)]
SERIES = item("s1", "Signal Fire", "Series", year=2021, unplayed=3)
SEASONS = [item("se1", "Season 1", "Season", IndexNumber=1, unplayed=3), item("se2", "Season 2", "Season", IndexNumber=2, unplayed=0)]
EPS = [item(f"e{k}", f"Episode {k}", "Episode", year=2021, mins=45, SeriesName="Signal Fire", SeriesId="s1", ParentIndexNumber=1, IndexNumber=k,
            played=(k == 1), pos=(13500000000 if k == 2 else 0)) for k in range(1, 5)]
VIEWS = [item("l1", "Movies", "CollectionFolder", CollectionType="movies"), item("l2", "Shows", "CollectionFolder", CollectionType="tvshows")]
ALL = {x["Id"]: x for x in MOVIES + [SERIES] + SEASONS + EPS + VIEWS}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body=b"", ctype="application/json"):
        if not isinstance(body, bytes): body = json.dumps(body).encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def authed(self):
        h = self.headers.get("Authorization", "") + self.headers.get("X-Emby-Token", "")
        return TOKEN in h
    def body(self):
        n = int(self.headers.get("Content-Length", 0)); raw = self.rfile.read(n) if n else b""
        try: return json.loads(raw or b"{}")
        except Exception: return {}
    def log(self, what):
        if ARGS.log:
            with open(ARGS.log, "a") as f: f.write(what + "\n")
    def do_any(self, method):
        u = urlparse(self.path); p = u.path; q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if p == "/System/Info/Public": return self.send(200, {"ServerName": "Mock Jellyfin", "ProductName": "Jellyfin Server", "Id": "mock1", "Version": "10.10.0"})
        if p == "/QuickConnect/Enabled": return self.send(200, b"true")
        if p == "/QuickConnect/Initiate": return self.send(200, {"Secret": "sekret", "Code": "482917", "Authenticated": False})
        if p == "/QuickConnect/Connect":
            with lock: polls["n"] += 1; ok = polls["n"] >= 2
            return self.send(200, {"Secret": q.get("secret"), "Code": "482917", "Authenticated": ok})
        if p == "/Users/AuthenticateWithQuickConnect": return self.send(200, {"AccessToken": TOKEN, "User": {"Id": UID, "Name": "jamie"}})
        if p == "/Users/AuthenticateByName":
            b = self.body()
            if b.get("Username") == "jamie" and b.get("Pw") == "pw": return self.send(200, {"AccessToken": TOKEN, "User": {"Id": UID, "Name": "jamie"}})
            return self.send(401, {"Message": "bad"})
        if p.startswith("/Items/") and "/Images/" in p: return self.send(200, IMG, "image/png")
        if not self.authed(): return self.send(401, {"Message": "unauthorised"})
        if p == "/UserViews": return self.send(200, {"Items": VIEWS})
        if p.endswith("/Items/Resume"): return self.send(200, {"Items": [MOVIES[1], EPS[1]]})
        if p == "/Shows/NextUp": return self.send(200, {"Items": [EPS[2]]})
        if p == "/Items/Latest": return self.send(200, MOVIES if q.get("ParentId") == "l1" else [SERIES])
        if p == "/Items":
            if q.get("SearchTerm"):
                hits = [x for x in MOVIES + [SERIES] if q["SearchTerm"].lower() in x["Name"].lower()]
                return self.send(200, {"Items": hits, "TotalRecordCount": len(hits)})
            pid = q.get("ParentId"); items = MOVIES if pid == "l1" else [SERIES] if pid == "l2" else []
            return self.send(200, {"Items": items, "TotalRecordCount": len(items)})
        if p.startswith("/Users/") and "/Items/" in p and not p.endswith("Resume"):
            it = ALL.get(p.rsplit("/", 1)[1]); return self.send(200, it) if it else self.send(404, {})
        if p.endswith("/Seasons"): return self.send(200, {"Items": SEASONS})
        if p.endswith("/Episodes"): return self.send(200, {"Items": EPS if q.get("SeasonId") == "se1" else []})
        if "/PlayedItems/" in p: self.log(f"PLAYED {method} {p.rsplit('/',1)[1]}"); return self.send(200, {})
        if p.startswith("/Sessions/Playing"):
            b = self.body(); kind = p.rsplit("/", 1)[1] if p != "/Sessions/Playing" else "start"
            self.log(f"REPORT {kind} item={b.get('ItemId')} ticks={b.get('PositionTicks')} paused={b.get('IsPaused')} play={b.get('PlaySessionId')}")
            return self.send(204)
        if p.startswith("/Videos/"): return self.send(200, b"\0" * 1024, "video/mp4")
        return self.send(404, {"Message": "no route " + p})
    def do_GET(self): self.do_any("GET")
    def do_POST(self): self.do_any("POST")
    def do_DELETE(self): self.do_any("DELETE")

ap = argparse.ArgumentParser(); ap.add_argument("--port", type=int, default=18096); ap.add_argument("--log", default="")
ARGS = ap.parse_args()
print(f"mock jellyfin on :{ARGS.port}  token={TOKEN}", flush=True)
ThreadingHTTPServer(("127.0.0.1", ARGS.port), H).serve_forever()
