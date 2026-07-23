#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sources="$root/sources"
clips="$root/clips"
references="$root/references"
manifest="$root/manifest.csv"

mkdir -p "$sources" "$clips" "$references"

download() {
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

sha256sum "$sources"/*.mov "$sources"/*.mkv > "$sources/SHA256SUMS"
sha256sum "$clips"/*.mp4 "$references"/*.mkv > "$root/CORPUS_SHA256SUMS"

printf 'Prepared %s input clips and %s native-reference clips.\n' \
    "$(find "$clips" -maxdepth 1 -name '*.mp4' | wc -l)" \
    "$(find "$references" -maxdepth 1 -name '*.mkv' | wc -l)"
