#!/bin/sh
# Offline media conversion for the PS Vita (called by tools/stage_data.sh).
#
#   music  m_*.m4a  ALAC 48 kHz stereo (no ALAC decoder on the Vita) -> m_*.wav PCM s16le 44.1 kHz
#          stereo, played by the SoundEngine BGM hooks (source/patch.c -> coreaudio.c sound_bgm_*).
#   video  *.mp4    MPEG-4 Part 2 Simple Profile 480x320 (the Vita decodes only H.264) -> H.264
#          Baseline + AAC-LC, same file name, played by source/video.c (sceAvcdec + FFmpeg AAC).
#   SFX    sounds.bar keeps its PCM WAVs (s16 mono 44.1 kHz): nothing to convert.
#
# Usage: tools/convert_media.sh <source .app> <staged .app>
# Re-running only converts files whose source is newer than the output.
set -e
SRC="$1"
DST="$2"
if [ ! -d "$SRC" ] || [ ! -d "$DST" ]; then
    echo "usage: $0 <source .app> <staged .app>" >&2
    exit 2
fi
command -v ffmpeg >/dev/null || { echo "[-] ffmpeg not found (brew install ffmpeg)" >&2; exit 1; }

newer() { [ ! -f "$2" ] || [ "$1" -nt "$2" ]; }

n_audio=0
for f in "$SRC"/*.m4a; do
    [ -f "$f" ] || continue
    case "$(basename "$f")" in ._*) continue ;; esac
    out="$DST/$(basename "${f%.m4a}").wav"
    if newer "$f" "$out"; then
        ffmpeg -nostdin -loglevel error -y -i "$f" -vn -acodec pcm_s16le -ar 44100 -ac 2 "$out.tmp.wav"
        mv "$out.tmp.wav" "$out"
        echo "    [audio] $(basename "$f") -> $(basename "$out")"
    fi
    n_audio=$((n_audio + 1))
done

n_video=0
for f in "$SRC"/*.mp4 "$SRC"/*.m4v; do
    [ -f "$f" ] || continue
    case "$(basename "$f")" in ._*) continue ;; esac
    out="$DST/$(basename "$f")"
    if newer "$f" "$out" || ! ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$out" | grep -q h264; then
        ffmpeg -nostdin -loglevel error -y -i "$f" -c:v libx264 -profile:v baseline -level 3.0 -pix_fmt yuv420p \
            -preset slow -crf 20 -c:a aac -b:a 128k -ar 44100 -ac 2 -movflags +faststart "$out.tmp.mp4"
        mv "$out.tmp.mp4" "$out"
        touch "$out"
        echo "    [video] $(basename "$f") -> H.264 baseline + AAC"
    fi
    n_video=$((n_video + 1))
done
echo "[+] media: $n_audio music tracks (WAV), $n_video video(s) (H.264) in $DST"
