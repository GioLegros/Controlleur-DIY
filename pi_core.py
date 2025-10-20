from flask import Flask, request, jsonify
from spotipy import Spotify
from spotipy.oauth2 import SpotifyOAuth
import json, threading, time

# --- Charger les clés externes ---
with open("spotify_keys.json") as f:
    keys = json.load(f)

app = Flask(__name__)

SPOTIFY_SCOPE = "user-read-playback-state user-modify-playback-state user-read-currently-playing"

sp = Spotify(auth_manager=SpotifyOAuth(
    client_id=keys["SPOTIFY_CLIENT_ID"],
    client_secret=keys["SPOTIFY_CLIENT_SECRET"],
    redirect_uri=keys["SPOTIFY_REDIRECT_URI"],
    scope=SPOTIFY_SCOPE,
    open_browser=False,
    cache_path=".cache"
))

@app.route("/spotify_now")
def spotify_now():
    cur = sp.current_playback() or {}
    item = cur.get("item") or {}
    return jsonify({
        "title": item.get("name", "—"),
        "artist": ", ".join([a["name"] for a in item.get("artists", [])]) if item else "—",
        "is_playing": cur.get("is_playing", False),
        "progress": cur.get("progress_ms", 0),
        "duration": (item.get("duration_ms") or 1),
        "art_url": (item.get("album", {}).get("images", [{}])[0].get("url", ""))
    })

@app.route("/spotify_cmd", methods=["POST"])
def spotify_cmd():
    cmd = (request.json or {}).get("cmd", "")
    try:
        if cmd == "playpause":
            cur = sp.current_playback()
            if cur and cur.get("is_playing"):
                sp.pause_playback()
            else:
                sp.start_playback()
        elif cmd == "next":
            sp.next_track()
        elif cmd == "prev":
            sp.previous_track()
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)})
    return jsonify({"ok": True})

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5005)
