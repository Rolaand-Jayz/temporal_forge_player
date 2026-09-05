#!/usr/bin/env bash
set -euo pipefail

# prepare_corpus.sh — materialize the reproducible benchmark corpus.
#
# Upstream: licensed public source videos and the scene/resolution choices at
# the bottom of this file. Downstream: ignored source clips, references, and
# manifest.csv consumed by performance/quality runners. This script prepares
# data only; it does not change the player or its reconstruction algorithms.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sources="$root/sources"
clips="$root/clips"
references="$root/references"
manifest="$root/manifest.csv"

mkdir -p "$sources" "$clips" "$references"

download() {
    # download: resume a missing/incomplete source file without replacing a
    # valid local copy. The caller supplies the public URL and exact path used
    # by the manifest.
    local url="$1"
    local output="$2"
    if [[ -s "$output" ]]; then
        return
    fi
    curl --location --fail --continue-at - --output "$output" "$url"
}

download \
    "https://download.blender.org/demo/movies/ToS/tearsofsteel_4k.mov" \
    "$sources/tears_of_steel_4k.mov"
download \
    "https://download.blender.org/durian/movies/Sintel.2010.4k.mkv" \
    "$sources/sintel_4k.mkv"
download \
    "https://download.blender.org/peach/bigbuckbunny_movies/big_buck_bunny_1080p_h264.mov" \
    "$sources/big_buck_bunny_1080p.mov"

printf '%s\n' \
    'clip_id,title,source_url,license,start_seconds,duration_seconds,width,height,quality,crf,path,reference_path' \
    > "$manifest"

encode_reference() {
    # encode_reference: make the high-resolution lossless-ish comparison clip
    # for a real scene. Quality runners compare against this reference after
    # matching the same time interval and output dimensions.
    local id="$1"
    local source="$2"
    local start="$3"
    local duration="$4"
    local output="$references/${id}_2160p_lossless.mkv"
    if [[ ! -s "$output" ]]; then
        ffmpeg -hide_banner -loglevel error -stats \
            -ss "$start" -i "$source" -t "$duration" -an \
            -vf "scale=3840:2160:force_original_aspect_ratio=decrease:flags=lanczos,pad=3840:2160:(ow-iw)/2:(oh-ih)/2:black,setsar=1" \
            -c:v libx264 -preset medium -crf 0 -pix_fmt yuv420p \
            -g 48 -keyint_min 48 -sc_threshold 0 "$output"
    fi
    printf '%s' "$output"
}

encode_variant() {
    # encode_variant: create one codec/resolution/quality variant and append
    # its provenance to manifest.csv. The manifest is the downstream contract
    # for every benchmark runner, so the output path and reference stay paired.
    local id="$1"
    local title="$2"
    local url="$3"
    local source="$4"
    local start="$5"
    local duration="$6"
    local width="$7"
    local height="$8"
    local quality="$9"
    local crf="${10}"
    local reference="${11}"
    local output="$clips/${id}_${width}x${height}_${quality}_crf${crf}.mp4"

    if [[ ! -s "$output" ]]; then
        ffmpeg -hide_banner -loglevel error -stats \
            -ss "$start" -i "$source" -t "$duration" -an \
            -vf "scale=${width}:${height}:force_original_aspect_ratio=decrease:flags=lanczos,pad=${width}:${height}:(ow-iw)/2:(oh-ih)/2:black,setsar=1" \
            -c:v libx264 -preset medium -crf "$crf" -pix_fmt yuv420p \
            -g 48 -keyint_min 48 -sc_threshold 0 -movflags +faststart "$output"
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$id" "$title" "$url" "CC-BY-3.0" "$start" "$duration" \
        "$width" "$height" "$quality" "$crf" "$output" "$reference" \
        >> "$manifest"
}

make_scene() {
    # make_scene: expand one real-world source interval into the common review
    # resolution and compression matrix. Keeping this list centralized makes
    # corpus growth auditable and prevents runners from inventing filenames.
    local id="$1"
    local title="$2"
    local url="$3"
    local source="$4"
    local start="$5"
    local duration="$6"
    local has_4k_reference="$7"
    local reference=""

    if [[ "$has_4k_reference" == "yes" ]]; then
        reference="$(encode_reference "$id" "$source" "$start" "$duration")"
    fi

    for resolution in 426x240 640x360 854x480 1280x720 1920x1080; do
        local width="${resolution%x*}"
        local height="${resolution#*x}"
        encode_variant "$id" "$title" "$url" "$source" "$start" "$duration" \
            "$width" "$height" high 12 "$reference"
        encode_variant "$id" "$title" "$url" "$source" "$start" "$duration" \
            "$width" "$height" medium 23 "$reference"
        encode_variant "$id" "$title" "$url" "$source" "$start" "$duration" \
            "$width" "$height" low 35 "$reference"
    done
}

# Deterministic synthetic scenes cover failure modes that are hard to isolate
# in natural footage: one-pixel edges, text, controlled motion, and near-black
# gradients. They are generated locally, so corpus preparation does not need
# another external asset and every regeneration is bit-for-bit reproducible.
make_synthetic_scene() {
    # make_synthetic_scene: generate controlled diagnostic media for automated
    # failure isolation. These assets may support engineering tests, but the
    # human-facing review harness filters them out by family name.
    local id="$1"
    local title="$2"
    local filter="$3"
    local source="$sources/${id}_source.mkv"
    if [[ ! -s "$source" ]]; then
        ffmpeg -hide_banner -loglevel error -stats \
            -f lavfi -i "${filter}" -t 10 -an \
            -c:v ffv1 -level 3 -g 1 -pix_fmt yuv420p "$source"
    fi
    make_scene "$id" "$title" "synthetic" "$source" 0 10 yes
}

# Native-aspect synthetic family. Keep these variants unpadded so the generic
# arbitrary-aspect path is measured against a true 4:3 reference rather than a
# 16:9 clip containing pillarbox/letterbox pixels.
make_synthetic_4_3_scene() {
    # make_synthetic_4_3_scene: create unpadded 4:3 diagnostics so aspect-ratio
    # handling can be tested independently from letterbox pixels. Like the
    # other synthetic family, it is benchmark-only and not review UI content.
    local id="$1"
    local title="$2"
    local filter="$3"
    local source="$sources/${id}_source.mkv"
    local reference="$references/${id}_native_lossless.mkv"
    if [[ ! -s "$source" ]]; then
        ffmpeg -hide_banner -loglevel error -stats \
            -f lavfi -i "${filter}" -t 10 -an \
            -c:v ffv1 -level 3 -g 1 -pix_fmt yuv420p "$source"
    fi
    if [[ ! -s "$reference" ]]; then
        ffmpeg -hide_banner -loglevel error -stats \
            -i "$source" -t 10 -an -c:v ffv1 -level 3 -g 1 \
            -pix_fmt yuv420p "$reference"
    fi
    for resolution in 640x480 800x600 1024x768 1280x960; do
        local width="${resolution%x*}"
        local height="${resolution#*x}"
        for quality_crf in 'high 12' 'medium 23' 'low 35'; do
            read -r quality crf <<< "$quality_crf"
            local output="$clips/${id}_${resolution}_${quality}_crf${crf}.mp4"
            if [[ ! -s "$output" ]]; then
                ffmpeg -hide_banner -loglevel error -stats \
                    -i "$source" -t 10 -an \
                    -vf "scale=${width}:${height}:flags=lanczos,setsar=1" \
                    -c:v libx264 -preset medium -crf "$crf" -pix_fmt yuv420p \
                    -g 48 -keyint_min 48 -sc_threshold 0 -movflags +faststart \
                    "$output"
            fi
            printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                "$id" "$title" "synthetic" "CC-BY-3.0" 0 10 \
                "$width" "$height" "$quality" "$crf" "$output" "$reference" \
                >> "$manifest"
        done
    done
}

tos_url="https://download.blender.org/demo/movies/ToS/tearsofsteel_4k.mov"
sintel_url="https://download.blender.org/durian/movies/Sintel.2010.4k.mkv"
bbb_url="https://download.blender.org/peach/bigbuckbunny_movies/big_buck_bunny_1080p_h264.mov"

make_scene tos_daylight "Tears of Steel daylight architecture and actor motion" \
    "$tos_url" "$sources/tears_of_steel_4k.mov" 245 10 yes
make_scene tos_debris "Tears of Steel bright VFX debris and hard edges" \
    "$tos_url" "$sources/tears_of_steel_4k.mov" 455 10 yes
make_scene bbb_grass "Big Buck Bunny foliage grass and character motion" \
    "$bbb_url" "$sources/big_buck_bunny_1080p.mov" 348 10 no
make_scene bbb_branches "Big Buck Bunny fast branch motion and fine foliage" \
    "$bbb_url" "$sources/big_buck_bunny_1080p.mov" 435 10 no
make_scene sintel_rooftop "Sintel rooftop action and high-contrast silhouettes" \
    "$sintel_url" "$sources/sintel_4k.mkv" 258 10 yes
make_scene sintel_cave "Sintel dark cave crystals smoke and texture" \
    "$sintel_url" "$sources/sintel_4k.mkv" 516 10 yes

make_synthetic_scene synthetic_edges_text \
    "Synthetic one-pixel edges and UI-like text" \
    "testsrc2=size=1920x1080:rate=30,drawbox=x=0:y=0:w=1920:h=1:color=white:t=fill,drawbox=x=0:y=0:w=1:h=1080:color=white:t=fill,drawbox=x=960:y=0:w=1:h=1080:color=red:t=fill,drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf:text='EDGE TEXT 0123456789':fontcolor=white:fontsize=42:x=80:y=80"
make_synthetic_scene synthetic_motion \
    "Synthetic moving high-contrast geometry" \
    "color=c=black:size=1920x1080:rate=30,drawbox=x='mod(t*420\,1800)':y=180:w=120:h=120:color=white:t=fill,drawbox=x='1800-mod(t*260\,1800)':y=720:w=90:h=90:color=red:t=fill,drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf:text='MOTION':fontcolor=gray:fontsize=48:x=700:y=500"
make_synthetic_scene synthetic_dark \
    "Synthetic dark gradient and shadow detail" \
    "gradients=s=1920x1080:rate=30:duration=10:c0=black:c1=0x202020,drawbox=x=300:y=300:w=500:h=300:color=0x101010:t=fill,drawbox=x=1100:y=500:w=400:h=260:color=0x404040:t=fill"

# A native 4:3 source is kept as a separate scene family so aspect-ratio and
# common 4:3 native-pack work can be measured without padding it into 16:9.
make_synthetic_4_3_scene synthetic_4_3 \
    "Synthetic 4:3 geometry and text" \
    "testsrc2=size=1440x1080:rate=30,drawbox=x=0:y=0:w=1440:h=1:color=white:t=fill,drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf:text='4\\:3 TEXT':fontcolor=white:fontsize=52:x=80:y=80"

sha256sum "$sources"/*.mov "$sources"/*.mkv > "$sources/SHA256SUMS"
sha256sum "$clips"/*.mp4 "$references"/*.mkv > "$root/CORPUS_SHA256SUMS"

printf 'Prepared %s input clips and %s native-reference clips.\n' \
    "$(find "$clips" -maxdepth 1 -name '*.mp4' | wc -l)" \
    "$(find "$references" -maxdepth 1 -name '*.mkv' | wc -l)"
