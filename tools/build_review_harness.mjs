#!/usr/bin/env node
// build_review_harness.mjs — build the human-facing still reviewer from the
// real benchmark result pool.
//
// Upstream: benchmark result PNGs and their filename metadata. Downstream: a
// standalone HTML page plus a sibling asset directory consumed by a browser.
// This tool intentionally owns only discovery, metadata resolution, and UI
// generation; it never runs reconstruction or rewrites benchmark images.
import fs from 'node:fs';
import path from 'node:path';

const input = process.argv[2] ?? path.resolve('temporal-forge-frame55-review-standalone.html');
const output = process.argv[3] ?? path.resolve('temporal-forge-frame55-review-standalone.html');
const assetOutputRoot = path.resolve(path.dirname(output), `${path.basename(output, path.extname(output))}-assets`);
// readEmbeddedAssetMap: recover the legacy embedded asset map without loading
// an entire multi-hundred-megabyte HTML file into a second temporary copy.
// Called by the rebuild path; downstream discovery replaces the old snapshot
// with the current real-world result pool.
function readEmbeddedAssetMap(file) {
  const fd = fs.openSync(file, 'r');
  const marker = Buffer.from('const embeddedAssets = Object.freeze(');
  const chunkSize = 8 * 1024 * 1024;
  const chunk = Buffer.allocUnsafe(chunkSize);
  let carry = Buffer.alloc(0);
  let start = -1;
  let fileOffset = 0;
  try {
    while (start < 0) {
      const count = fs.readSync(fd, chunk, 0, chunk.length, fileOffset);
      if (!count) break;
      const data = Buffer.concat([carry, chunk.subarray(0, count)]);
      const found = data.indexOf(marker);
      if (found >= 0) {
        start = fileOffset - carry.length + found + marker.length;
        carry = data.subarray(found + marker.length);
        break;
      }
      carry = data.subarray(Math.max(0, data.length - marker.length + 1));
      fileOffset += count;
    }
    if (start < 0) throw new Error(`Could not find embedded asset map in ${file}`);
    let json = Buffer.from(carry);
    let end = json.indexOf(Buffer.from(');'));
    let position = start + carry.length;
    while (end < 0) {
      const count = fs.readSync(fd, chunk, 0, chunk.length, position);
      if (!count) throw new Error(`Embedded asset map is truncated in ${file}`);
      json = Buffer.concat([json, chunk.subarray(0, count)]);
      position += count;
      end = json.indexOf(Buffer.from(');'));
    }
    return json.subarray(0, end).toString('utf8');
  } finally {
    fs.closeSync(fd);
  }
}
const embeddedSource = fs.existsSync(input) ? readEmbeddedAssetMap(input) : '{}';
const match = embeddedSource.match(/^(\{[\s\S]*\})$/);
if (!match) throw new Error(`Could not find embedded asset map in ${input}`);
const allAssets = JSON.parse(match[1]);
// The old standalone embedded a partial snapshot. The distributable now keeps
// the images beside the HTML so the browser loads only the two selected files.
// The source map is still read for backwards-compatible rebuilds, but it is not
// copied into the generated page.
const embeddedAssets = {};
// TFORGE_REVIEW_RESULTS_ROOT keeps discovery deterministic in CI and lets a
// reviewer build from a checked-out result folder without changing this
// generator. Production defaults to the repository's real benchmark pool.
const configuredResultsRoot = process.env.TFORGE_REVIEW_RESULTS_ROOT;
const resultsRoot = path.resolve(
  configuredResultsRoot ?? 'benchmarks/video_corpus/results',
);
const reviewRoots = fs.existsSync(resultsRoot)
  ? fs.readdirSync(resultsRoot, {withFileTypes: true})
    .filter(entry => entry.isDirectory() && /^review_.*_frame\d+$/i.test(entry.name))
    .map(entry => path.join(resultsRoot, entry.name))
    .sort()
  : [];
// An explicit discovery root is a complete input contract. Keeping the
// repository's default pool out of that mode prevents unrelated local capture
// runs from changing a fixture or a reviewer-provided corpus. The normal
// distributable build still includes the checked-in quality-frame pool.
if (!configuredResultsRoot)
  reviewRoots.unshift(path.resolve('benchmarks/video_corpus/results/quality_frames'));
const excludedReviewAsset = name => {
  return /^(?:synthetic(?:_|\.)|source(?:_|\.)|supersampled_aa(?:_|\.)|intel_|os_|s_)/i.test(name) || /lanczos_roundtrip_fsr/i.test(name) || /frame55_/i.test(name) || /_(?:difference|metadata|gpu_raw)\.png$/i.test(name);
};
// The human reviewer uses the practical 640x360 input tier. The benchmark
// directory contains many historical controls and intermediate probes, so the
// distributable admits only the primary result, one named candidate, four
// explicitly captured opt-ins, and the matching native reference. This keeps
// every visible choice backed by a real file without recreating the old wall
// of hundreds of near-duplicate buttons.
const reviewFamilyForName = name => {
  if (/_640x360_high_crf12_f48\.png$/i.test(name)) return 'temporal-forge';
  if (/_640x360_high_crf12_f48_cross_control\.png$/i.test(name)) return 'candidate-1';
  if (/_640x360_high_crf12_f48_(?:cas0p00|cas0p02|cross_direct_unjittered|cross_jitter_off)\.png$/i.test(name)) return 'opt-in';
  if (/_reference_1280x720_f48\.png$/i.test(name)) return 'native';
  // Curated review captures use an explicit input/output token. That token is
  // the provenance boundary that keeps the large historical result pool out
  // of the human-facing harness while allowing 480p/720p/1080p tiers and all
  // requested output sizes to coexist in one manifest.
  if (/_input\d+x\d+_(?:to\d+x\d+_)?(?:native-input|native-reference|temporal-forge|candidate-[1-6]|opt-in-[1-4]|lanczos|bicubic)(?:\.|_)/i.test(name)) return 'curated';
  return null;
};
// Feature-stack order is part of the manifest key contract.  The UI uses the
// same order when a reviewer toggles features, so a valid combination resolves
// regardless of the order in which its buttons were clicked.
const featureOrder = ['stable-base', 'learned-only', 'learned-blend', 'detail-residual', 'adaptive-sharpen', 'tone', 'experimental'];
const canonicalFeatureStack = values => [...new Set(values)].sort((a, b) =>
  (featureOrder.indexOf(a) - featureOrder.indexOf(b)) || a.localeCompare(b));
const pngDimensions = file => { const b = fs.readFileSync(file); return b.length >= 24 && b.readUInt32BE(0) === 0x89504e47 ? `${b.readUInt32BE(16)}x${b.readUInt32BE(20)}` : null; };
const sourceByName = new Map();
const addSourceAssets = (root, names) => {
  for (const name of names) {
    if (!name.endsWith('.png') || excludedReviewAsset(name) || !reviewFamilyForName(name) || sourceByName.has(name)) continue;
    if (/(?:_medium_|_low_)/i.test(name)) continue;
    const sourcePath = path.join(root, name);
    sourceByName.set(name, sourcePath);
  }
};
for (const root of reviewRoots) {
  if (fs.existsSync(root)) addSourceAssets(root, fs.readdirSync(root));
}
const assetFamily = name => {
  const lower = name.toLowerCase();
  if (lower.includes('_reference_')) return 'native';
  if (/_frame\d+\.png$/i.test(name)) return 'native-input';
  if (/_lanczos(?:_review)?\.png$/i.test(lower)) return 'lanczos-control';
  if (/_bicubic(?:_review)?\.png$/i.test(lower)) return 'bicubic-control';
  if (/_bilinear(?:_review)?\.png$/i.test(lower)) return 'bilinear-control';
  if (/_lanczos_native_fsr_review\.png$/i.test(lower)) return 'lanczos-native-fsr';
  if (/_bicubic_prefsr_temporal_forge_bicubic_review\.png$/i.test(lower)) return 'bicubic-prefsr-bicubic';
  if (lower.endsWith('_f48.png') || lower.includes('_current') || lower.endsWith('_temporal_forge_review.png')) return 'temporal-forge';
  return 'experimental';
};
// parseAssetMetadata: convert one discovered filename into the structured
// fields consumed by both selectors. The image itself remains untouched;
// this is only the review manifest's provenance layer.
const parseAssetMetadata = (name, sourcePath) => {
  const lower = name.toLowerCase();
  const sceneMatch = name.match(/^(?<scene>tos_daylight|tos_debris|bbb_grass|bbb_branches|sintel_rooftop|sintel_cave)(?:_|$)/i);
  if (!sceneMatch) return null;
  const imageResolution = pngDimensions(sourcePath);
  if (!imageResolution) return null;
  // Generic-path probes may be two columns narrower than their intended
  // review target so they avoid a fixed native shape. Keep the physical PNG
  // dimensions for provenance, but resolve the human-facing output selector
  // to the intended display size so 1278x720 reads as 1280x720 and 1918x1080
  // reads as 1920x1080. The viewer scales both sides onto the shared intended
  // canvas; 1:1 metadata still exposes the actual source pixels.
  const intendedResolution = {
    '1278x720': '1280x720',
    '1918x1080': '1920x1080',
  }[imageResolution] ?? imageResolution;
  const reference = /(?:native-reference|_reference(?:_\d+x\d+)?_f\d+)/i.test(name);
  const target = (name.match(/_to_?(\d+x\d+)/i) ?? [])[1] ?? null;
  const explicitInput = (name.match(/_input(\d+x\d+)(?:_|\.)/i) ?? [])[1] ?? null;
  const inputResolution = explicitInput ?? (reference
    ? '640x360'
    : (name.match(/_(\d+x\d+)(?:_|\.)/) ?? [])[1] ?? imageResolution);
  // The filename uses the historical capture token "current", but the
  // reviewer must never expose that implementation-era wording. Internally
  // classify it as the maintained Temporal Forge result instead.
  const temporalForgeMarker = /(?:_|-)current(?:[-_.]|$)/i.test(lower);
  const nativeInput = !reference && !target && !lower.includes('stage') &&
    !temporalForgeMarker &&
    (/_frame\d+\.png$/i.test(name) || /_high_crf\d+_review\.png$/i.test(name) || /native-input/i.test(lower));
  const derivedControl = /_(?:bicubic|lanczos|difference|metadata)\.png$/i.test(lower);
  const temporalForgeName = !derivedControl && !lower.includes('_bicubic_prefsr_') &&
    !lower.includes('_lanczos_prefsr_') &&
    (temporalForgeMarker || lower.endsWith('_f48.png') ||
      lower.endsWith('_temporal_forge_review.png') ||
      /_temporal[-_]forge(?:\.png|_)/i.test(lower));
  const experimentalName = !temporalForgeName &&
    (lower.includes('stagea-') || lower.includes('base_only') ||
      lower.includes('direct_blend') || lower.includes('stageb-') ||
      /stage[acdef](?:[-_]|$)/i.test(lower) || lower.includes('stageb-') ||
      lower.includes('staged-') ||
      lower.includes('stagee') || lower.includes('stagef') ||
      lower.includes('post-campaign'));
  let technique = 'experimental';
  if (reference) technique = 'native';
  else if (nativeInput) technique = 'native-input';
  else if (/bicubic_prefsr_temporal_forge_bicubic_review\.png$/i.test(lower)) technique = 'bicubic-prefsr-bicubic';
  else if (/bicubic_prefsr_temporal_forge_lanczos_review\.png$/i.test(lower)) technique = 'bicubic-prefsr-lanczos';
  else if (/lanczos_prefsr_temporal_forge_review\.png$/i.test(lower)) technique = 'lanczos-prefsr';
  else if (temporalForgeName) technique = 'temporal-forge';
  else if (/cross_control\.png$/i.test(lower)) technique = 'candidate-1';
  else if (/cas0p00\.png$/i.test(lower)) technique = 'opt-in-1';
  else if (/cas0p02\.png$/i.test(lower)) technique = 'opt-in-2';
  else if (/cross_direct_unjittered\.png$/i.test(lower)) technique = 'opt-in-3';
  else if (/cross_jitter_off\.png$/i.test(lower)) technique = 'opt-in-4';
  else if (/_input\d+x\d+_(?:to\d+x\d+_)?candidate-1(?:\.|_)/i.test(lower)) technique = 'candidate-1';
  else if (/_input\d+x\d+_(?:to\d+x\d+_)?candidate-2(?:\.|_)/i.test(lower)) technique = 'candidate-2';
  else if (/_input\d+x\d+_(?:to\d+x\d+_)?candidate-3(?:\.|_)/i.test(lower)) technique = 'candidate-3';
  else if (/_input\d+x\d+_(?:to\d+x\d+_)?candidate-4(?:\.|_)/i.test(lower)) technique = 'candidate-4';
  else if (/_input\d+x\d+_(?:to\d+x\d+_)?candidate-5(?:\.|_)/i.test(lower)) technique = 'candidate-5';
  else if (/_input\d+x\d+_(?:to\d+x\d+_)?candidate-6(?:\.|_)/i.test(lower)) technique = 'candidate-6';
  else if (/_input\d+x\d+_(?:to\d+x\d+_)?opt-in-([1-4])(?:\.|_)/i.test(lower)) technique = `opt-in-${lower.match(/opt-in-([1-4])/)?.[1]}`;
  else if (/lanczos_native_fsr_review\.png$/i.test(lower)) technique = 'lanczos-native-fsr';
  else if (/lanczos(?:_review)?\.png$/i.test(lower) || /_input\d+x\d+_(?:to\d+x\d+_)?lanczos(?:\.png|_)/i.test(lower)) technique = 'lanczos';
  else if (/bicubic(?:_review)?\.png$/i.test(lower) || /_input\d+x\d+_(?:to\d+x\d+_)?bicubic(?:\.png|_)/i.test(lower)) technique = 'bicubic';
  else if (/native-input/i.test(lower)) technique = 'native-input';
  else if (/bilinear(?:_review)?\.png$/i.test(lower)) technique = 'bilinear-control';
  if (!['native', 'native-input', 'lanczos', 'bicubic', 'temporal-forge', 'candidate-1', 'candidate-2', 'candidate-3', 'candidate-4', 'candidate-5', 'candidate-6', 'opt-in-1', 'opt-in-2', 'opt-in-3', 'opt-in-4'].includes(technique)) return null;

  const featureStack = [];
  if (technique === 'experimental') {
    if (/base_only|baseonly|stable_base|stageb-/i.test(lower)) featureStack.push('stable-base');
    if (/learned_only/i.test(lower)) featureStack.push('learned-only');
    if (/direct_blend|blend/i.test(lower)) featureStack.push('learned-blend');
    if (/stagec(?:[-_]|$)|stagef-res|-res-|_res-|residual/i.test(lower)) featureStack.push('detail-residual');
    if (/adaptive|sharpen/i.test(lower)) featureStack.push('adaptive-sharpen');
    if (/exposure_|tone_|gamma_/i.test(lower)) featureStack.push('tone');
    if (!featureStack.length) featureStack.push('experimental');
  }
  const baseFilter = /mitchell/i.test(lower) ? 'Mitchell' :
    /catmull_rom/i.test(lower) ? 'Catmull-Rom' :
    /lanczos2/i.test(lower) ? 'Lanczos2' :
    /bilinear/i.test(lower) ? 'Bilinear' :
    /bicubic/i.test(lower) ? 'Bicubic' :
    /lanczos/i.test(lower) ? 'Lanczos' : null;
  const modifiers = {
    baseFilter,
    blendStrength: null,
    learnedStrength: null,
    residualStrength: null,
    sharpeningMode: null,
    sharpeningStrength: null,
    toneConfiguration: null,
  };
  const blendStrength = lower.match(/(?:direct_blend|blend)(?:_|-)?(\d{3})/);
  if (blendStrength) {
    const value = Number(blendStrength[1]) / 100;
    if (lower.includes('direct_blend')) modifiers.learnedStrength = value;
    else modifiers.blendStrength = value;
  } else if (lower.includes('learned_only')) {
    modifiers.learnedStrength = 1;
  }
  const residualStrength = lower.match(/(?:detail_)?residual(?:_strength)?(?:_|-)?(\d{3})/);
  if (residualStrength) modifiers.residualStrength = Number(residualStrength[1]) / 100;
  const adaptiveSharpen = lower.match(/(?:adaptive_)?sharpen(?:_strength)?(?:_|-)?(\d{3})/i) ??
    lower.match(/adaptive_s(\d{3})/i) ?? lower.match(/sharpen_s(\d{3})/i);
  if (adaptiveSharpen) {
    modifiers.sharpeningMode = 'Adaptive';
    modifiers.sharpeningStrength = Number(adaptiveSharpen[1]) / 100;
  }
  if (lower.includes('adaptive')) modifiers.sharpeningMode = 'Adaptive';
  const toneParts = [];
  const exposure = lower.match(/(?:exposure_|tone_)(m|p)?(\d{3})/);
  if (exposure) toneParts.push(`exposure ${exposure[1] === 'm' ? '-' : exposure[1] === 'p' ? '+' : ''}${(Number(exposure[2]) / 1000).toFixed(3)} EV`);
  const gamma = lower.match(/gamma_(\d{3})/);
  if (gamma) toneParts.push(`gamma ${(Number(gamma[1]) / 100).toFixed(2)}`);
  if (toneParts.length) modifiers.toneConfiguration = toneParts.join(' · ');
  const outputResolution = target
    ? ({'1278x720': '1280x720', '1918x1080': '1920x1080'}[target] ?? target)
    : intendedResolution;
  const experimentId = technique === 'candidate-1' || technique === 'candidate-2' || technique === 'candidate-3' || technique === 'candidate-4' || technique === 'candidate-5' || technique === 'candidate-6'
    ? technique
    : technique.startsWith('opt-in-')
    ? technique
    : experimentalName
    ? name.replace(/^.*(?:frame|f)48_?/i, '').replace(/\.png$/i, '') || null
    : null;
  const canonicalStack = canonicalFeatureStack(featureStack);
  return {
    assetName: name,
    assetPath: `${path.basename(assetOutputRoot)}/${name}`,
    scene: sceneMatch.groups.scene.toLowerCase(),
    family: technique,
    inputResolution,
    imageResolution,
    intendedResolution,
    outputResolution,
    technique,
    // Native/reference/control assets have no experimental stack. Null keeps
    // the manifest truthful and lets the dependent UI omit meaningless
    // modifier controls instead of treating an empty string as a selection.
    featureStack: canonicalStack.length ? canonicalStack : null,
    featureStackKey: canonicalStack.length ? canonicalStack.join('+') : null,
    experimentId,
    ...modifiers,
  };
};

const assetManifest = [...sourceByName.entries()]
  .sort(([a], [b]) => a.localeCompare(b))
  .map(([name, sourcePath]) => parseAssetMetadata(name, sourcePath))
  .filter(Boolean);
if (!assetManifest.length) throw new Error('No real-world review assets remain after metadata parsing');
const externalAssets = assetManifest.map(asset => ({
  name: asset.assetName,
  src: asset.assetPath,
  imageResolution: asset.imageResolution,
  intendedResolution: asset.intendedResolution,
}));
if (!externalAssets.length) throw new Error('No real-world review assets remain after synthetic filtering');

const html = String.raw`<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Temporal Forge still review</title>
  <style>
    :root{color-scheme:dark;--bg:#0d1218;--panel:#151c25;--raised:#1a2330;--line:#2b3746;--strong:#405065;--text:#f1f5f9;--muted:#9ba9ba;--focus:#a9c9e8;--shadow:0 18px 48px #0004}
    *{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}button,input,select{font:inherit}button:focus-visible,input:focus-visible,select:focus-visible{outline:2px solid var(--focus);outline-offset:2px}select{width:100%;min-height:38px;border:1px solid var(--strong);border-radius:8px;padding:8px 10px;background:var(--raised);color:var(--text)}select option{background:var(--panel);color:var(--text)}
    header{position:sticky;top:0;z-index:5;padding:18px clamp(18px,4vw,48px);background:#0d1218f2;backdrop-filter:blur(14px);border-bottom:1px solid var(--line)}.header-inner{max-width:1540px;margin:auto;display:flex;justify-content:space-between;gap:24px;align-items:end}.eyebrow{margin:0 0 5px;color:var(--muted);font-size:11px;font-weight:700;letter-spacing:.12em;text-transform:uppercase}h1{margin:0 0 6px;font-size:clamp(22px,2.4vw,32px);line-height:1.15;letter-spacing:-.025em}header p:not(.eyebrow){max-width:850px;margin:0;color:var(--muted)}.header-actions{display:flex;gap:8px;flex-wrap:wrap;justify-content:flex-end}button{border:1px solid var(--strong);border-radius:8px;padding:8px 12px;background:transparent;color:var(--text);cursor:pointer}button:hover{background:var(--raised);border-color:var(--text)}
    main{max-width:1540px;margin:auto;padding:clamp(22px,4vw,48px) clamp(18px,4vw,48px) 24px}.toolbar{display:flex;justify-content:space-between;gap:12px;align-items:center;margin-bottom:16px;color:var(--muted);font-size:12px}.comparison{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px;margin-top:18px}.panel{min-width:0;background:var(--panel);border:1px solid var(--line);border-radius:14px;box-shadow:var(--shadow);overflow:hidden}.panel-head{padding:17px 18px 12px;border-bottom:1px solid var(--line)}.panel-label{margin:0 0 3px;font-size:18px;font-weight:750;letter-spacing:.01em}.panel-context{margin:0;color:var(--muted);font-size:12px}.selectors{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;padding:14px 18px 12px}.field{min-width:0}.field label{display:block;margin-bottom:6px;color:var(--muted);font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.05em}.state-buttons{display:flex;flex-wrap:wrap;gap:6px}.state-button{border-color:var(--strong);background:#101720;color:var(--muted);padding:7px 9px;border-radius:7px;font-size:12px}.state-button:hover{color:var(--text)}.state-button[aria-pressed="true"]{border-color:var(--focus);background:#30465e;color:var(--text);box-shadow:inset 0 0 0 1px #dbeafe44}.modifiers{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;padding:0 18px 14px}.modifiers:empty{display:none}.result-meta{padding:12px 18px 18px;color:var(--muted);font-size:12px}.result-meta strong{display:block;margin-bottom:2px;color:var(--text);font-size:14px}.badge{display:inline-block;margin-left:6px;padding:2px 6px;border:1px solid var(--strong);border-radius:99px;color:var(--muted);font-size:10px}.compare-wrap{margin-top:0}.compare-stage{position:relative;width:100%;aspect-ratio:16/9;overflow:hidden;background:#080b0f;border:1px solid var(--strong);border-radius:12px;cursor:ew-resize;user-select:none;touch-action:none}.compare-stage img{position:absolute;inset:0;width:100%;height:100%;object-fit:contain;background:#080b0f;pointer-events:none}.compare-stage .compare-after{clip-path:inset(0 0 0 var(--split,50%))}.compare-handle{position:absolute;top:0;bottom:0;left:var(--split,50%);width:3px;transform:translateX(-50%);background:#fff;box-shadow:0 0 0 1px #0008;pointer-events:auto;cursor:ew-resize;z-index:2}.compare-handle::after{content:'↔';position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);padding:6px 8px;border:1px solid #111;border-radius:14px;background:#fff;color:#111;font-weight:700}.compare-range{display:block;width:100%;margin:10px 0 0;accent-color:#dbe5ef;cursor:ew-resize}.compare-legend{display:flex;justify-content:space-between;gap:12px;margin-top:6px;color:var(--muted);font-size:11px}.compare-caption{margin:0 0 7px;color:var(--muted);font-size:12px}.empty{padding:28px 18px;color:var(--muted)}footer{max-width:1540px;margin:auto;padding:0 clamp(18px,4vw,48px) 34px;color:var(--muted);font-size:12px}
    .viewer[hidden]{display:none}.viewer-backdrop{position:fixed;inset:0;z-index:20;display:grid;place-items:center;padding:12px;background:#06090de6;backdrop-filter:blur(16px)}.viewer-panel{width:min(1500px,100%);height:min(94vh,1100px);display:flex;flex-direction:column;overflow:hidden;background:var(--panel);border:1px solid var(--strong);border-radius:16px;box-shadow:0 28px 90px #000c}.viewer-header{display:flex;justify-content:space-between;gap:20px;padding:16px 20px;border-bottom:1px solid var(--line)}.viewer-header h2{margin:0 0 3px;font-size:18px}.viewer-header p:last-child{margin:0;color:var(--muted);font-size:12px}.viewer-close{width:36px;height:36px;padding:0;font-size:24px}.viewer-tools{display:flex;gap:7px;flex-wrap:wrap;padding:10px 20px;border-bottom:1px solid var(--line);align-items:center}.viewer-tools .status{margin-left:auto;color:var(--muted);font-size:12px}.viewer-stage{min-height:0;flex:1;overflow:auto;display:grid;place-items:center;padding:18px;background:#090d12}.viewer-stage.pixel-mode{display:block}.viewer-compare{position:relative;flex:none;background:#080b0f;margin-bottom:30px}.viewer-compare:not(.pixel-mode){max-width:100%;max-height:100%}.viewer-compare img{position:absolute;inset:0;width:100%;height:100%;object-fit:contain;pointer-events:none}.viewer-compare .compare-after{clip-path:inset(0 0 0 var(--split,50%))}.viewer-compare.pixel-mode img{width:auto;height:auto;max-width:none;max-height:none;object-fit:none;image-rendering:pixelated}.viewer-compare .compare-handle{position:absolute}.viewer-compare .compare-range{position:absolute;left:0;bottom:-28px;width:100%;margin:0}.viewer-note{padding:8px 20px;color:var(--muted);font-size:11px;border-top:1px solid var(--line)}.magnifier{position:absolute;z-index:5;width:220px;height:220px;border:3px solid #f4eee2;border-radius:50%;overflow:hidden;pointer-events:none;box-shadow:0 10px 35px #000b,0 0 0 1px #111}.magnifier img{position:absolute;inset:auto;width:100%;height:100%;max-width:none;max-height:none;object-fit:fill;image-rendering:pixelated}.magnifier .compare-after{clip-path:inset(0 0 0 var(--split,50%))}.magnifier-crosshair{position:absolute;inset:50% auto auto 50%;width:12px;height:12px;transform:translate(-50%,-50%);border:1px solid #fff;border-radius:50%;z-index:6}.viewer-compare.lens-active{cursor:none}.viewer-compare.lens-active .compare-handle{cursor:ew-resize}.viewer-compare.lens-active .compare-handle::after{cursor:ew-resize}.viewer-note kbd{padding:1px 4px;border:1px solid var(--strong);border-radius:4px;background:var(--raised)}body.viewer-open{overflow:hidden}@media(max-width:900px){.comparison{grid-template-columns:1fr}}@media(max-width:560px){.selectors,.modifiers{grid-template-columns:1fr}.header-inner{align-items:flex-start;flex-direction:column}.header-actions{justify-content:flex-start}}
  </style>
  <style>.viewer-compare.pixel-mode img{max-width:none;max-height:none;object-fit:contain;image-rendering:pixelated}.advanced-options{margin:0 18px 14px;border-top:1px solid var(--line)}.advanced-options summary{padding:11px 0;color:var(--muted);font-size:12px;font-weight:700;cursor:pointer}.advanced-options[open] summary{color:var(--text)}.advanced-options .modifiers{padding:2px 0}</style>
  <style id="product-review-theme">
    :root{--bg:#0b0d10;--panel:#12161b;--raised:#1b222b;--line:#2b333d;--strong:#596575;--text:#f4f1ea;--muted:#9da5ae;--focus:#d9b56d;--left:#76b7ff;--right:#e6a76d;--shadow:0 24px 80px #0009}
    *{box-sizing:border-box}body{background:radial-gradient(ellipse at 50% -20%,#26313d 0,#101419 34%,var(--bg) 72%);color:var(--text);font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;letter-spacing:.005em}
    header{position:relative;padding:30px clamp(20px,5vw,72px) 24px;background:linear-gradient(180deg,#11161ce8,#0b0d10e8);border-bottom:1px solid #323b46;backdrop-filter:blur(18px)}.header-inner{max-width:1420px;align-items:center}.eyebrow{color:var(--focus);font-size:10px;letter-spacing:.18em}h1{font-size:clamp(28px,4vw,52px);font-weight:650;letter-spacing:-.055em;line-height:1.02;margin-bottom:10px}header p:not(.eyebrow){max-width:680px;font-size:14px;line-height:1.6;color:#b7bdc4}.header-actions{align-self:flex-start}.header-actions button{border-radius:999px;padding:10px 15px;background:#161c23;border-color:#485463;font-size:12px}.header-actions button:first-child{background:var(--focus);border-color:var(--focus);color:#15120c;font-weight:750}.header-actions button:hover{transform:translateY(-1px);transition:transform .15s ease}
    main{max-width:1420px;padding:28px clamp(20px,5vw,72px) 24px}.toolbar{margin-bottom:20px;padding:0;color:#aeb5bc}.toolbar span:first-child{color:#f3eee4;font-size:13px;letter-spacing:.02em}.compare-wrap{padding:0}.compare-caption{margin:0 0 10px;color:#c6cbd0;font-size:12px}.compare-stage{aspect-ratio:16/9;border:1px solid #667486;border-radius:10px;box-shadow:0 18px 70px #000b;background:#050607}.compare-handle{width:4px;background:#fff8e8;box-shadow:0 0 0 1px #111,0 0 24px #fff6}.compare-handle::after{content:'↔';padding:7px 10px;border:0;border-radius:999px;background:#f4eee2;color:#202020;box-shadow:0 4px 18px #0009}.compare-range{height:16px;margin:12px 0 0;accent-color:var(--focus)}.compare-legend{margin-top:8px;color:#aeb5bc;text-transform:uppercase;letter-spacing:.08em;font-size:10px}.comparison{gap:14px;margin-top:22px;align-items:start}.panel{border:1px solid #343e49;border-radius:12px;background:linear-gradient(180deg,#161b21,#11151a);box-shadow:0 18px 55px #0006}.panel:first-child{border-top:2px solid var(--left)}.panel:last-child{border-top:2px solid var(--right)}.panel-head{padding:17px 18px 13px;background:linear-gradient(90deg,#1b232c,#151a20);border-bottom:1px solid #303946}.panel-label{font-size:13px;letter-spacing:.16em}.panel:first-child .panel-label{color:var(--left)}.panel:last-child .panel-label{color:var(--right)}.panel-context{color:#aeb5bc;line-height:1.5}.selectors{gap:10px;padding:14px 18px}.field label{color:#aeb8c2;font-size:10px;letter-spacing:.12em}.field select{min-height:40px;background:#0d1116;border-color:#485462;border-radius:7px;color:#f3f0e9}.field select:hover{border-color:#8291a1}.state-buttons{gap:5px}.state-button{border-radius:5px;padding:6px 8px;font-size:11px;background:#0d1116;border-color:#3e4a58;color:#aeb7c0}.state-button[aria-pressed="true"]{background:#333b45;border-color:var(--focus);color:#fff8e8;box-shadow:none}.advanced-options{margin:0 18px 12px;border-top:1px solid #303946}.advanced-options summary{padding:12px 0;color:#c7cdd2;font-size:11px;letter-spacing:.08em;text-transform:uppercase}.modifiers{gap:10px}.result-meta{padding:13px 18px 18px;color:#aeb5bc;border-top:1px solid #242c35;font-size:11px;line-height:1.7}.result-meta strong{color:#f4f1ea;font-size:14px;font-weight:600}.badge{border-color:#596575;color:#d6dbe0;text-transform:uppercase;letter-spacing:.06em}.viewer-panel{border-color:#667486;background:#12161b}.viewer-tools{background:#171d24}.viewer-tools button{border-radius:999px}.viewer-tools button[data-view="pixel"]{border-color:var(--focus);color:var(--focus)}.viewer-note{color:#aeb5bc;background:#101419}@media(max-width:800px){.header-inner{align-items:flex-start;flex-direction:column}.header-actions{align-self:stretch;justify-content:flex-start}.comparison{grid-template-columns:1fr}}@media(max-width:560px){main{padding-top:20px}.selectors,.modifiers{grid-template-columns:1fr}.panel-head{padding-left:15px;padding-right:15px}}
  </style>
  <style id="button-only-review-controls">
    .state-button:disabled{opacity:.28;cursor:not-allowed;filter:saturate(.25);border-style:dashed;color:#7f8994}
    .state-button:disabled:hover{background:#0d1116;border-color:#3e4a58;color:#7f8994;transform:none}
    .selectors .field{min-height:92px}
    .selectors .state-buttons{align-content:flex-start}
  </style>
</head>
<body>
  <header><div class="header-inner"><div><p class="eyebrow">Temporal Forge · standalone review</p><h1>Mirrored still comparison</h1><p>Each side resolves one real-world asset from independent, data-driven selectors. Click either image for fit or true 1:1 pixel inspection.</p></div><div class="header-actions"><button id="copy-link" type="button">Copy comparison link</button><button id="reset" type="button">Reset comparison</button></div></div></header>
  <main><div class="toolbar"><span id="asset-count"></span><span>Selections are saved in the URL hash for sharing.</span></div><div id="compare-wrap" class="compare-wrap"></div><div id="comparison" class="comparison"></div></main>
  <footer>Standalone review page · asset folder: ${path.basename(assetOutputRoot)} · benchmark source images are not modified</footer>
  <div id="viewer" class="viewer" hidden aria-hidden="true"><div class="viewer-backdrop"><section class="viewer-panel" role="dialog" aria-modal="true" aria-labelledby="viewer-title"><div class="viewer-header"><div><p class="eyebrow">Pixel inspection</p><h2 id="viewer-title">Image</h2><p id="viewer-meta"></p></div><button id="viewer-close" class="viewer-close" type="button" aria-label="Close enlarged view">×</button></div><div class="viewer-tools"><button data-view="fit" type="button">Fit to view</button><button data-view="pixel" type="button">100% / 1:1 (highest)</button><button data-view="in" type="button">Zoom in</button><button data-view="out" type="button">Zoom out</button><button data-view="reset" type="button">Reset zoom</button><button id="lens-toggle" type="button" aria-pressed="false">Lens: off</button><span id="viewer-status" class="status"></span></div><div id="viewer-stage" class="viewer-stage"></div><div class="viewer-note">Scroll over the image to zoom. Fit shows the whole comparison; 1:1 uses the highest selected canvas as one CSS pixel per source pixel. With Lens on, move the round lens over the image; <kbd>Ctrl</kbd>-click turns it off so the divider can be grabbed normally.</div></section></div></div>
  <script>
    const embeddedAssets = Object.freeze(__EMBEDDED_ASSETS__);
    const assetManifest = Object.freeze(__ASSET_MANIFEST__);
    const externalAssets = Object.freeze(__EXTERNAL_ASSETS__);
    const labels = {tos_daylight:'Tears of Steel — daylight',tos_debris:'Tears of Steel — debris',bbb_grass:'Big Buck Bunny — grass',bbb_branches:'Big Buck Bunny — branches',sintel_rooftop:'Sintel — rooftop',sintel_cave:'Sintel — cave'};
    const techniqueLabels = {'native':'Native reference','native-input':'Native input','lanczos':'Lanczos','bicubic':'Bicubic','temporal-forge':'Temporal Forge','candidate-1':'Candidate #1','candidate-2':'Candidate #2','candidate-3':'Candidate #3','candidate-4':'Candidate #4','candidate-5':'Candidate #5','candidate-6':'Candidate #6','opt-in-1':'Opt-in #1','opt-in-2':'Opt-in #2','opt-in-3':'Opt-in #3','opt-in-4':'Opt-in #4'};
    function assetId(a){return [a.scene,a.inputResolution??'',a.outputResolution??'',a.technique,a.featureStackKey??'',a.baseFilter??'',a.blendStrength??'',a.learnedStrength??'',a.residualStrength??'',a.sharpeningMode??'',a.sharpeningStrength??'',a.toneConfiguration??'',a.experimentId??'',a.assetName].join('|')}
    function sceneLabel(scene){return labels[scene] ?? scene.replaceAll('_',' ')}
    function embeddedAssetSource(assetName){
      const embedded=embeddedAssets[assetName];
      if(!embedded)return null;
      if(embedded.startsWith('data:'))return embedded;
      const payload=document.querySelector('script[data-tforge-hash="'+embedded+'"]');
      return payload?('data:'+payload.dataset.mime+';base64,'+payload.textContent):null;
    }
    function parseAssets(){
      const seen=new Set();
      return assetManifest.filter(asset=>{
        if(seen.has(asset.assetName)) return false;
        seen.add(asset.assetName);
        return true;
      }).map(asset=>({...asset,src:embeddedAssetSource(asset.assetName)??asset.assetPath,id:assetId(asset)}));
    }
    const assets=Object.freeze(parseAssets());
    const byId=new Map(assets.map(a=>[a.id,a]));
    const unique=(items,key)=>[...new Set(items.map(x=>x[key]).filter(v=>v!==null&&v!==undefined))];
    const resolutionRank=value=>{const match=String(value).match(/^(\d+)x(\d+)$/);return match?Number(match[1])*Number(match[2]):Number.MAX_SAFE_INTEGER;};
    const experimentLabel=value=>{const raw=String(value);const clean=raw.replace(/^stage([a-z0-9]+)-[^-]+-\d+-/i,'Stage $1 · ').replace(/^(?:post-campaign|quality-campaign-baseline)[- ]*/i,'Baseline · ').replaceAll('_',' ').replaceAll('-',' ');return clean.replace(/\bbase only\b/i,'Base only').replace(/\blearned only\b/i,'Learned only').replace(/\bdirect blend (\d{3})\b/i,(_,v)=>'Learned blend · '+(Number(v)/10)+'%').replace(/\bexposure ([mp])(\d{3})\b/i,(_,sign,v)=>'Exposure '+(sign.toLowerCase()==='m'?'-':'+')+(Number(v)/1000)+' EV').replace(/\bgamma (\d{3})\b/i,(_,v)=>'Gamma '+(Number(v)/1000).toFixed(2));};
    const valueLabel=(field,value)=>{if(field==='technique')return techniqueLabels[value]??String(value);if(field==='inputResolution'||field==='outputResolution'){const match=String(value).match(/^(\d+)x(\d+)$/);return match?match[1]+' × '+match[2]:String(value);}if(field==='featureStackKey')return featureLabels[value]??String(value);if(field==='learnedStrength'||field==='blendStrength'||field==='residualStrength'||field==='sharpeningStrength'){const number=Number(value);return Number.isFinite(number)?(number*100).toFixed(0)+'%':String(value);}if(field==='toneConfiguration')return String(value);if(field==='experimentId')return experimentLabel(value);return String(value);};
    const orderedValues=(field,values)=>values.sort((a,b)=>{if(field==='inputResolution'||field==='outputResolution'){const rank=resolutionRank(a)-resolutionRank(b);return rank||String(a).localeCompare(String(b),undefined,{numeric:true});}if(field==='scene')return sceneLabel(a).localeCompare(sceneLabel(b));if(field==='technique'){const order=['native-input','lanczos','bicubic','temporal-forge','candidate-1','candidate-2','candidate-3','candidate-4','candidate-5','candidate-6','opt-in-1','opt-in-2','opt-in-3','opt-in-4','native'];return (order.indexOf(a)-order.indexOf(b))||String(a).localeCompare(String(b));}if(field==='experimentId')return experimentLabel(a).localeCompare(experimentLabel(b),undefined,{numeric:true});return String(a).localeCompare(String(b),undefined,{numeric:true});});
    const first=(arr)=>arr[0];
    const defaultContext=first(assets.filter(a=>a.scene==='tos_daylight'&&a.inputResolution==='426x240'&&a.technique==='temporal-forge'))??first(assets);
    const defaultLeft=first(assets.filter(a=>a.scene===defaultContext.scene&&a.inputResolution===defaultContext.inputResolution&&a.technique==='candidate-1'))??defaultContext;
    const state={left:defaultLeft.id,right:defaultContext.id};
    function readHash(){const p=new URLSearchParams(location.hash.slice(1)); for(const side of ['left','right']) if(byId.has(p.get(side))) state[side]=p.get(side)}
    function writeHash(){const p=new URLSearchParams({left:state.left,right:state.right}); history.replaceState(null,'',__BT__\__DOLLAR__{location.pathname}\__DOLLAR__{location.search}#\__DOLLAR__{p}__BT__)}
    function candidates(side,field){const selected=byId.get(state[side]); if(!selected)return[]; return assets.filter(a=>a.scene===selected.scene&& (field==='scene'||a.inputResolution===selected.inputResolution || (selected.technique==='native-reference'&&field==='inputResolution')) && (field!=='outputResolution'||a.outputResolution===selected.outputResolution));}
    function validFor(selection){return assets.filter(a=>a.scene===selection.scene&&a.inputResolution===selection.inputResolution&&a.outputResolution===selection.outputResolution&&a.technique===selection.technique&&['baseFilter','learnedStrength','residualStrength','sharpeningMode','sharpeningStrength','toneConfiguration','experimentId'].every(k=>(a[k]??null)===(selection[k]??null)))}
    function reconcile(side,preferred={}){
      const current=byId.get(state[side])??defaultContext;
      const patch={...current,...preferred};
      const sceneAssets=assets.filter(x=>x.scene===patch.scene);
      const choose=(list,key,value)=>list.find(x=>x[key]===value);
      let a=choose(sceneAssets,'inputResolution',patch.inputResolution)??first(sceneAssets)??current;
      a=choose(sceneAssets.filter(x=>x.inputResolution===a.inputResolution),'outputResolution',patch.outputResolution)??first(sceneAssets.filter(x=>x.inputResolution===a.inputResolution))??a;
      a=choose(sceneAssets.filter(x=>x.inputResolution===a.inputResolution&&x.outputResolution===a.outputResolution),'technique',patch.technique)??first(sceneAssets.filter(x=>x.inputResolution===a.inputResolution&&x.outputResolution===a.outputResolution))??a;
      const modifierKeys=['featureStackKey','baseFilter','blendStrength','learnedStrength','residualStrength','sharpeningMode','sharpeningStrength','toneConfiguration','experimentId'];
      const exact=sceneAssets.filter(x=>x.inputResolution===a.inputResolution&&x.outputResolution===a.outputResolution&&x.technique===a.technique&&modifierKeys.every(k=>(x[k]??null)===(patch[k]??null)));
      a=first(exact)??a; state[side]=a.id;
    }
    const dimensionFields=['scene','inputResolution','outputResolution','technique'];
    function optionsFor(side,field){
      const current=byId.get(state[side]); if(!current)return[];
      if(field==='featureStackKey')return stackOptions(side);
      const dimensionIndex=dimensionFields.indexOf(field);
      const candidates=assets.filter(a=>{
        if(dimensionIndex>=0) return dimensionFields.slice(0,dimensionIndex).every(name=>(a[name]??null)===(current[name]??null));
        if(!dimensionFields.every(name=>(a[name]??null)===(current[name]??null))) return false;
        return modifierFields.every(([name])=>name===field||(a[name]??null)===(current[name]??null));
      });
      const values=orderedValues(field,unique(candidates,field));
      return values.map(value=>({value,label:field==='scene'?sceneLabel(value):valueLabel(field,value),available:true}));
    }
    const featureLabels={'stable-base':'Stable base','learned-only':'Learned only','learned-blend':'Learned blend','detail-residual':'Detail residual','adaptive-sharpen':'Adaptive sharpen','tone':'Tone correction','experimental':'Experimental'};
    const featureOrder=['stable-base','learned-only','learned-blend','detail-residual','adaptive-sharpen','tone','experimental'];
    const canonicalFeatureStack=values=>[...new Set(values)].sort((a,b)=>(featureOrder.indexOf(a)-featureOrder.indexOf(b))||a.localeCompare(b));
    function stackOptions(side){const current=byId.get(state[side]);if(!current)return[];if(current.technique!=='experimental')return[];const pool=assets.filter(a=>a.scene===current.scene&&a.inputResolution===current.inputResolution&&a.outputResolution===current.outputResolution&&a.technique==='experimental');const choices=unique(assets,'featureStackKey').filter(Boolean).flatMap(key=>key.split('+')).filter((value,index,array)=>array.indexOf(value)===index);choices.sort((a,b)=>(featureOrder.indexOf(a)-featureOrder.indexOf(b))||a.localeCompare(b));return choices.map(feature=>({value:feature,label:featureLabels[feature]??feature,available:pool.some(a=>a.featureStackKey?.split('+').includes(feature))})).filter(option=>option.available);}
    function makeStackButtons(side){const wrap=document.createElement('div');wrap.className='field stack-field';const l=document.createElement('label');l.textContent='Experimental feature stack';const group=document.createElement('div');group.className='state-buttons';group.setAttribute('role','group');group.setAttribute('aria-label',__BT__\__DOLLAR__{side} Experimental feature stack__BT__);const current=byId.get(state[side]);const activeStack=current.featureStack??[];const pool=assets.filter(a=>a.scene===current.scene&&a.inputResolution===current.inputResolution&&a.outputResolution===current.outputResolution&&a.technique==='experimental');for(const {value:feature,label} of stackOptions(side)){const active=activeStack.includes(feature);const next=[...new Set(active?activeStack.filter(x=>x!==feature):[...activeStack,feature])];const canonicalNext=canonicalFeatureStack(next);const valid=pool.some(a=>a.featureStackKey===canonicalNext.join('+'));const button=document.createElement('button');button.type='button';button.className='state-button';button.textContent=label;button.setAttribute('aria-pressed',String(active));button.disabled=!valid;button.title=valid?'Toggle this feature in the stack':'No captured asset for this stack in the selected context';button.addEventListener('click',()=>{if(valid){reconcile(side,{featureStackKey:canonicalNext.join('+')});render();}});group.append(button)}wrap.append(l,group);return wrap;}
    function makeStateSelect(side,field,label){const values=optionsFor(side,field);if(!values.length)return null;const wrap=document.createElement('div');wrap.className='field';const l=document.createElement('label');l.textContent=label;const select=document.createElement('select');select.setAttribute('aria-label',__BT__\__DOLLAR__{side} \__DOLLAR__{label}__BT__);const selected=byId.get(state[side]);for(const o of values){const option=document.createElement('option');option.value=String(o.value);option.textContent=o.label;option.disabled=o.available===false;option.selected=String(o.value)===String(selected[field]);if(o.available===false)option.textContent+=' · unavailable';select.append(option)}select.addEventListener('change',()=>{const selectedOption=values.find(o=>String(o.value)===select.value);if(selectedOption){reconcile(side,{[field]:selectedOption.value});render();}});wrap.append(l,select);return wrap;}
    function makeStateButtons(side,field,label){if(field==='featureStackKey')return makeStackButtons(side);const wrap=document.createElement('div');wrap.className='field';const l=document.createElement('label');l.textContent=label;const group=document.createElement('div');group.className='state-buttons';group.setAttribute('role','group');group.setAttribute('aria-label',__BT__\__DOLLAR__{side} \__DOLLAR__{label}__BT__);const values=optionsFor(side,field);const selected=byId.get(state[side]);for(const o of values){const button=document.createElement('button');button.type='button';button.className='state-button';button.textContent=o.label;button.disabled=o.available===false;button.title=o.available===false?'No captured asset for the selected LEFT/RIGHT context':'Select this asset value';button.setAttribute('aria-pressed',String(String(o.value)===String(selected[field])));button.addEventListener('click',()=>{reconcile(side,{[field]:o.value});render();});group.append(button)}wrap.append(l,group);return wrap;}
    const modifierFields=[['featureStackKey','Experimental feature stack'],['baseFilter','Base filter'],['blendStrength','Blend strength'],['learnedStrength','Learned strength'],['residualStrength','Residual strength'],['sharpeningMode','Sharpening'],['sharpeningStrength','Sharpen strength'],['toneConfiguration','Tone / exposure'],['experimentId','Experiment / configuration']];
    function makePanel(side){const a=byId.get(state[side]);const panel=document.createElement('article');panel.className='panel';panel.innerHTML=__BT__<div class="panel-head"><p class="panel-label">__DOLLAR__{side.toUpperCase()}</p><p class="panel-context">Choose the image context first. Advanced quality controls appear only when applicable.</p></div>__BT__;const selectors=document.createElement('div');selectors.className='selectors';for(const [f,l] of [['scene','Scene'],['inputResolution','Input resolution'],['outputResolution','Output resolution'],['technique','Upscale technique']]){const field=makeStateSelect(side,f,l);if(field)selectors.append(field);}panel.append(selectors);const applicable=modifierFields.filter(([f])=>optionsFor(side,f).length);if(applicable.length){const details=document.createElement('details');details.className='advanced-options';const summary=document.createElement('summary');summary.textContent='Advanced quality options';details.append(summary);const modifiers=document.createElement('div');modifiers.className='modifiers';for(const [f,l] of applicable)modifiers.append(makeStateButtons(side,f,l));details.append(modifiers);panel.append(details);}const meta=document.createElement('div');meta.className='result-meta';meta.innerHTML=__BT__<strong>__DOLLAR__{sceneLabel(a.scene)} <span class="badge">__DOLLAR__{techniqueLabels[a.technique]??a.technique}</span></strong>__DOLLAR__{a.inputResolution?__BT____DOLLAR__{valueLabel('inputResolution',a.inputResolution)} → __BT__:''}__DOLLAR__{valueLabel('outputResolution',a.outputResolution)} · actual pixels __DOLLAR__{valueLabel('imageResolution',a.imageResolution)}__BT__;const modifiersText=modifierFields.map(([f,l])=>a[f]===null||a[f]===undefined||a[f]===''?null:__BT____DOLLAR__{l}: __DOLLAR__{valueLabel(f,a[f])}__BT__).filter(Boolean);if(modifiersText.length)meta.innerHTML+=__BT__<br>__DOLLAR__{modifiersText.join(' · ')}__BT__;panel.append(meta);return panel;}
    function makeComparison(viewerMode=false){const left=byId.get(state.left),right=byId.get(state.right);const wrap=document.createElement('div');wrap.className=viewerMode?'viewer-compare':'compare-stage';wrap.style.setProperty('--split','50%');const before=document.createElement('img'),after=document.createElement('img');before.src=left.src;after.src=right.src;before.alt='LEFT: '+sceneLabel(left.scene);after.alt='RIGHT: '+sceneLabel(right.scene);before.className='compare-before';after.className='compare-after';const handle=document.createElement('div');handle.className='compare-handle';const range=document.createElement('input');range.type='range';range.min=0;range.max=100;range.value=50;range.className='compare-range';range.setAttribute('aria-label','Divider position between LEFT and RIGHT');let dragged=false;const setSplitFromEvent=e=>{const rect=wrap.getBoundingClientRect();const value=Math.max(0,Math.min(100,((e.clientX-rect.left)/rect.width)*100));range.value=String(value);wrap.style.setProperty('--split',value+'%');};const update=()=>wrap.style.setProperty('--split',range.value+'%');range.addEventListener('input',update);const start=e=>{dragged=false;wrap.setPointerCapture?.(e.pointerId);setSplitFromEvent(e);const move=event=>{dragged=true;setSplitFromEvent(event)};const end=()=>{wrap.removeEventListener('pointermove',move);wrap.removeEventListener('pointerup',end);wrap.removeEventListener('pointercancel',end)};wrap.addEventListener('pointermove',move);wrap.addEventListener('pointerup',end);wrap.addEventListener('pointercancel',end)};handle.addEventListener('pointerdown',e=>{e.preventDefault();start(e)});wrap.addEventListener('pointerdown',e=>{if(e.target===range||e.target===handle)return;start(e)});wrap.append(before,after,handle);
      // The lens owns no pointer events, so the divider remains draggable
      // beneath it. Ctrl-click is handled by the wrapper to turn it off.
      if(viewerMode){const lens=document.createElement('div');lens.className='magnifier';lens.hidden=true;const lensBefore=before.cloneNode();const lensAfter=after.cloneNode();const crosshair=document.createElement('span');crosshair.className='magnifier-crosshair';lens.append(lensBefore,lensAfter,crosshair);wrap.append(lens);wrap._lens={element:lens,before:lensBefore,after:lensAfter};wrap.addEventListener('mousemove',e=>updateLens(e));wrap.addEventListener('click',e=>{if(e.ctrlKey)setLensEnabled(false);});}
      if(!viewerMode){wrap.addEventListener('click',e=>{if(e.target!==range&&!dragged)openViewer();});const caption=document.createElement('p');caption.className='compare-caption';caption.textContent='Grab the divider to sweep between LEFT and RIGHT · click the image for enlarged inspection';const legend=document.createElement('div');legend.className='compare-legend';legend.innerHTML='<span>LEFT · '+sceneLabel(left.scene)+'</span><span>RIGHT · '+sceneLabel(right.scene)+'</span>';const box=document.createElement('div');box.append(caption,wrap,range,legend);return box;}wrap.append(range);return wrap;}
    function render(){reconcile('left');reconcile('right');writeHash();document.getElementById('comparison').replaceChildren(makePanel('left'),makePanel('right'));document.getElementById('compare-wrap').replaceChildren(makeComparison());document.getElementById('asset-count').textContent=__BT____DOLLAR__{assets.length} valid real-world assets · __DOLLAR__{unique(assets,'scene').length} scenes__BT__}
    const viewer=document.getElementById('viewer'),stage=document.getElementById('viewer-stage'),viewerTitle=document.getElementById('viewer-title'),viewerMeta=document.getElementById('viewer-meta'),viewerStatus=document.getElementById('viewer-status'),lensToggle=document.getElementById('lens-toggle');let viewerCompare=null,zoom=0,lensEnabled=false;
    function intendedCanvas(left,right){const parse=value=>{const match=String(value??'').match(/^(\d+)x(\d+)$/);return match?{width:Number(match[1]),height:Number(match[2])}:null;};const sizes=[left,right].map(asset=>parse(asset?.intendedResolution??asset?.outputResolution)).filter(Boolean);return sizes.length?sizes.reduce((best,size)=>size.width*size.height>best.width*best.height?size:best):null;}
    function updateLens(e){if(!lensEnabled||!viewerCompare)return;const lens=viewerCompare._lens;if(!lens)return;const rect=viewerCompare.getBoundingClientRect();const x=Math.max(0,Math.min(rect.width,e.clientX-rect.left)),y=Math.max(0,Math.min(rect.height,e.clientY-rect.top));const diameter=220,mag=3;lens.element.style.left=(x-diameter/2)+'px';lens.element.style.top=(y-diameter/2)+'px';for(const image of [lens.before,lens.after]){image.style.width=(rect.width*mag)+'px';image.style.height=(rect.height*mag)+'px';image.style.left=(diameter/2-x*mag)+'px';image.style.top=(diameter/2-y*mag)+'px';}lens.element.style.setProperty('--split',getComputedStyle(viewerCompare).getPropertyValue('--split')||'50%');}
    function setLensEnabled(enabled){lensEnabled=Boolean(enabled)&&zoom===0&&Boolean(viewerCompare);if(viewerCompare?._lens){viewerCompare._lens.element.hidden=!lensEnabled;viewerCompare.classList.toggle('lens-active',lensEnabled);}lensToggle.setAttribute('aria-pressed',String(lensEnabled));lensToggle.textContent=lensEnabled?'Lens: on':'Lens: off';lensToggle.disabled=zoom!==0;}
    function updateViewer(){if(!viewerCompare)return;const images=[...viewerCompare.querySelectorAll(':scope > img')];const canvas=intendedCanvas(byId.get(state.left),byId.get(state.right));if(!canvas)return;const width=canvas.width,height=canvas.height;const pixelMode=zoom!==0;viewerCompare.classList.toggle('pixel-mode',pixelMode);const scale=zoom||1;viewerCompare.style.width=(width*scale)+'px';viewerCompare.style.height=(height*scale)+'px';for(const image of images){image.style.width=(width*scale)+'px';image.style.height=(height*scale)+'px';}const scaleLabel=zoom===0?'Fit to view':zoom===1?'100% / 1:1 highest canvas':Math.round(scale*100)+'% highest canvas';viewerStatus.textContent=scaleLabel+' · shared canvas '+width+'×'+height+' · lower-resolution side enlarged with nearest-neighbor pixels';if(zoom!==0&&lensEnabled)setLensEnabled(false);else setLensEnabled(lensEnabled);}
    function openViewer(){const left=byId.get(state.left),right=byId.get(state.right);viewerTitle.textContent='LEFT / RIGHT split inspection';viewerMeta.textContent=sceneLabel(left.scene)+' vs '+sceneLabel(right.scene)+' · divider remains draggable';stage.replaceChildren();viewerCompare=makeComparison(true);stage.append(viewerCompare);zoom=0;lensEnabled=false;viewer.hidden=false;viewer.setAttribute('aria-hidden','false');document.body.classList.add('viewer-open');const imgs=viewerCompare.querySelectorAll(':scope > img');imgs.forEach(img=>img.addEventListener('load',updateViewer));updateViewer();document.getElementById('viewer-close').focus()}
    function closeViewer(){viewer.hidden=true;viewer.setAttribute('aria-hidden','true');stage.replaceChildren();viewerCompare=null;document.body.classList.remove('viewer-open')}
    document.getElementById('viewer-close').addEventListener('click',closeViewer);document.querySelector('.viewer-backdrop').addEventListener('click',e=>{if(e.target===e.currentTarget)closeViewer()});document.addEventListener('keydown',e=>{if(!viewer.hidden&&e.key==='Escape')closeViewer()});stage.addEventListener('wheel',e=>{if(!viewerCompare)return;e.preventDefault();if(zoom===0)zoom=e.deltaY<0?1.25:.8;else zoom*=e.deltaY<0?1.2:(1/1.2);zoom=Math.max(.25,Math.min(16,zoom));updateViewer();},{passive:false});lensToggle.addEventListener('click',()=>setLensEnabled(!lensEnabled));document.querySelectorAll('[data-view]').forEach(b=>b.addEventListener('click',()=>{if(b.dataset.view==='fit')zoom=0;if(b.dataset.view==='pixel')zoom=1;if(b.dataset.view==='in')zoom=zoom===0?1.5:Math.min(16,zoom*1.5);if(b.dataset.view==='out')zoom=zoom===0?.8:Math.max(.25,zoom/1.5);if(b.dataset.view==='reset')zoom=1;updateViewer()}));
    document.getElementById('copy-link').addEventListener('click',async()=>{writeHash();try{await navigator.clipboard.writeText(location.href);document.getElementById('copy-link').textContent='Link copied';setTimeout(()=>document.getElementById('copy-link').textContent='Copy comparison link',1400)}catch{window.prompt('Copy this comparison URL:',location.href)}});document.getElementById('reset').addEventListener('click',()=>{state.left=defaultLeft.id;state.right=defaultContext.id;render()});
    // The reviewer uses buttons for every choice. The older generator code above
    // remains structurally compatible, but these assignments make the emitted
    // interface button-only and keep unavailable values visible as disabled.
    optionsFor = function(side,field){
      const current=byId.get(state[side]);
      if(!current)return[];
      if(field==='featureStackKey')return stackOptions(side);
      const dimensionIndex=dimensionFields.indexOf(field);
      const context=assets.filter(asset=>dimensionFields.every(name=>(asset[name]??null)===(current[name]??null)));
      if(dimensionIndex<0 && !context.some(asset=>asset[field]!==null&&asset[field]!==undefined&&asset[field]!==''))return[];
      const values=orderedValues(field,unique(assets,field));
      return values.map(value=>{
        const available=assets.some(asset=>{
          if(dimensionIndex>=0){
            return dimensionFields.slice(0,dimensionIndex).every(name=>(asset[name]??null)===(current[name]??null)) && (asset[field]??null)===value;
          }
          return dimensionFields.every(name=>(asset[name]??null)===(current[name]??null)) && modifierFields.every(([name])=>name===field||(asset[name]??null)===(current[name]??null)) && (asset[field]??null)===value;
        });
        return {value,label:field==='scene'?sceneLabel(value):valueLabel(field,value),available};
      });
    };
    stackOptions = function(side){
      const current=byId.get(state[side]);
      if(!current||current.technique!=='experimental')return[];
      const pool=assets.filter(asset=>asset.scene===current.scene&&asset.inputResolution===current.inputResolution&&asset.outputResolution===current.outputResolution&&asset.technique==='experimental');
      const choices=unique(assets,'featureStackKey').filter(Boolean).flatMap(key=>key.split('+')).filter((value,index,array)=>array.indexOf(value)===index);
      choices.sort((a,b)=>(featureOrder.indexOf(a)-featureOrder.indexOf(b))||a.localeCompare(b));
      return choices.map(feature=>({value:feature,label:featureLabels[feature]??feature,available:pool.some(asset=>asset.featureStackKey?.split('+').includes(feature))}));
    };
    makeStateSelect = function(side,field,label){return makeStateButtons(side,field,label);};
    function syncMainComparisonCanvas(){
      const wrap=document.querySelector('.compare-stage');
      if(!wrap)return;
      const images=[...wrap.querySelectorAll('img')];
      const apply=()=>{
        const canvas=intendedCanvas(byId.get(state.left),byId.get(state.right));
        if(!canvas)return;
        const width=canvas.width;
        const height=canvas.height;
        wrap.style.aspectRatio=width+'/'+height;
        wrap.dataset.canvasResolution=width+'x'+height;
        images.forEach(image=>{
          image.style.width='100%';
          image.style.height='100%';
          image.style.objectFit='fill';
        });
      };
      images.forEach(image=>image.addEventListener('load',apply,{once:true}));
      apply();
    }
    const renderWithSharedCanvas=render;
    render=function(){renderWithSharedCanvas();syncMainComparisonCanvas();};
    document.querySelector('h1').textContent='Temporal Forge results';
    document.querySelector('header p:not(.eyebrow)').textContent='Choose a scene, size, and result type with the buttons. Grey buttons mean that result was not captured for the selected context.';
    readHash();render();
  </script>
</body>
</html>`;
const rendered = html.replaceAll('__BT__','`').replaceAll('\\__DOLLAR__{','${').replaceAll('__DOLLAR__{','${')
  .replace('__EMBEDDED_ASSETS__', JSON.stringify(embeddedAssets))
  .replace('__ASSET_MANIFEST__', JSON.stringify(assetManifest));
const marker = '__EXTERNAL_ASSETS__';
const markerIndex = rendered.indexOf(marker);
if (markerIndex < 0) throw new Error('Could not find external asset placeholder');
const fd = fs.openSync(output, 'w');
try {
  fs.writeSync(fd, rendered.slice(0, markerIndex));
  fs.writeSync(fd, '[');
  externalAssets.forEach((asset, index) => {
    if (index) fs.writeSync(fd, ',');
    fs.writeSync(fd, JSON.stringify(asset));
  });
  fs.writeSync(fd, ']');
  fs.writeSync(fd, rendered.slice(markerIndex + marker.length));
} finally {
  fs.closeSync(fd);
}
fs.mkdirSync(assetOutputRoot, {recursive:true});
const allowedAssetNames = new Set(externalAssets.map(asset => asset.name));
for (const name of fs.readdirSync(assetOutputRoot)) {
  if (!allowedAssetNames.has(name)) fs.unlinkSync(path.join(assetOutputRoot, name));
}
for (const asset of externalAssets) {
  const sourcePath = sourceByName.get(asset.name);
  const targetPath = path.join(assetOutputRoot, asset.name);
  if (!fs.existsSync(targetPath) || fs.statSync(targetPath).size !== fs.statSync(sourcePath).size) fs.copyFileSync(sourcePath, targetPath);
}
console.log(`wrote ${output} with ${externalAssets.length} real-world assets in ${assetOutputRoot}`);
