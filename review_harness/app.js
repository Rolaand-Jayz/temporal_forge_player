'use strict';

const FALLBACK_PAIRS=[[360,480],[360,720],[360,1080],[480,720],[480,1080],[480,1440],[720,1080],[720,1440],[720,2160],[1080,1440],[1080,2160]];
const CAMPAIGN=window.__tforgeCampaign||{campaignId:'quality-campaign-20260902',frame:48,pairs:FALLBACK_PAIRS.map(([input,output])=>({input,output}))};
const LEGAL_PAIRS=CAMPAIGN.pairs.map(item=>[Number(item.input),Number(item.output)]);
const INPUTS=[...new Set(LEGAL_PAIRS.map(([input])=>input))];
const OUTPUTS=[...new Set(LEGAL_PAIRS.map(([,output])=>output))];
const SCENES=[['tos_daylight','Tears of Steel · daylight'],['tos_debris','Tears of Steel · debris'],['sintel_rooftop','Sintel · rooftop'],['sintel_cave','Sintel · cave']];
const MULTIPLIERS=[[200,'2×'],[225,'2.25×'],[250,'2.5×'],[275,'2.75×'],[300,'3×']];
const FALLBACK_CAS_ARMS=[{id:'resolve_cas20',label:'CAS 0.20 before downsampling'},{id:'external_post_cas20',label:'CAS 0.20 after downsampling'},{id:'no_cas',label:'No CAS sharpening'}];
const CAS_PLACEMENTS=(CAMPAIGN.downsamplingArms||FALLBACK_CAS_ARMS).map(arm=>[arm.id,arm.label]);
const VIEW_MODES=[['delivery','Delivery image'],['native','Reconstruction grid']];
const PATHS=[['current','Temporal Forge · current'],['direct','FSR direct target'],['supersample','FSR supersampled'],['nativeaa','NativeAA supersampled'],['bilinear','Bilinear + CAS .20'],['lanczos','Conventional Lanczos'],['bicubic','Conventional bicubic']];
const METHODS=['current_cas20','base_only_bilinear_cas20','fsr_direct_cas20',...MULTIPLIERS.flatMap(([value])=>CAS_PLACEMENTS.map(([placement])=>`fsr_${value}x_downsample_${placement}`)),...CAS_PLACEMENTS.map(([placement])=>`fsr_nativeaa_downsample_${placement}`),'conventional_lanczos','conventional_bicubic'];
const CATALOG=new Map(((window.__tforgeCatalog&&window.__tforgeCatalog.assets)||[]).map(item=>[[item.scene,Number(item.frame||CAMPAIGN.frame),Number(item.input),Number(item.output),item.method,item.view].join('|'),item]));
const state={left:{scene:'tos_daylight',frame:Number(CAMPAIGN.frame||48),input:720,output:1080,path:'current',multiplier:200,placement:'resolve_cas20',view:'delivery'},right:{scene:'tos_daylight',frame:Number(CAMPAIGN.frame||48),input:720,output:1080,path:'lanczos',multiplier:225,placement:'external_post_cas20',view:'delivery'},split:50,zoom:100};
const $=id=>document.getElementById(id);
const esc=value=>String(value).replace(/[&<>"']/g,char=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));
const outputsForInput=input=>LEGAL_PAIRS.filter(([candidate])=>candidate===Number(input)).map(([,output])=>output);

function selectedMethod(selection){
  if(selection.path==='current')return'current_cas20';
  if(selection.path==='direct')return'fsr_direct_cas20';
  if(selection.path==='nativeaa')return`fsr_nativeaa_downsample_${selection.placement}`;
  if(selection.path==='bilinear')return'base_only_bilinear_cas20';
  if(selection.path==='lanczos')return'conventional_lanczos';
  if(selection.path==='bicubic')return'conventional_bicubic';
  return`fsr_${selection.multiplier}x_downsample_${selection.placement}`;
}
function normalizeSelection(selection){
  const outputs=outputsForInput(selection.input);
  if(!outputs.includes(Number(selection.output)))selection.output=outputs[0];
  selection.method=selectedMethod(selection);
  if(!METHODS.includes(selection.method))throw new Error(`unknown method: ${selection.method}`);
  if(selection.view==='native'&&!selection.method.startsWith('fsr_'))selection.view='delivery';
  return selection;
}
function canonicalFilename(selection){
  normalizeSelection(selection);
  const scene=selection.scene.toLowerCase().replace(/[^a-z0-9]+/g,'_').replace(/^_+|_+$/g,'');
  const frame=String(selection.frame).padStart(4,'0');
  if(!/^\d{4}$/.test(frame))throw new Error('frame must be four digits');
  if(!LEGAL_PAIRS.some(([input,output])=>input===Number(selection.input)&&output===Number(selection.output)))throw new Error('resolution pair is outside the active campaign');
  const suffix=selection.view==='native'?'__native':'';
  return`scene-${scene}__frame-${frame}__in-${selection.input}p__method-${selection.method}__out-${selection.output}p${suffix}.png`;
}
function catalogEntry(selection){return CATALOG.get([selection.scene,Number(selection.frame),Number(selection.input),Number(selection.output),selection.method,selection.view].join('|'))||null}
function methodLabel(selection){
  const path=PATHS.find(([id])=>id===selection.path)?.[1]||selection.method;
  if(selection.path==='supersample')return`${path} · ${MULTIPLIERS.find(([id])=>id===Number(selection.multiplier))?.[1]} · ${CAS_PLACEMENTS.find(([id])=>id===selection.placement)?.[1]}`;
  if(selection.path==='nativeaa')return`${path} · ${CAS_PLACEMENTS.find(([id])=>id===selection.placement)?.[1]}`;
  return path;
}
function deliveryWidth(height){return Number(height)===480?854:Number(height)*16/9}
function evenDimension(value){const rounded=Math.round(value);return rounded%2===0?rounded:rounded+1}
function workingGrid(selection){
  const entry=catalogEntry(selection);
  if(entry&&entry.width&&entry.height)return`${entry.width}×${entry.height}`;
  if(!selection.method.startsWith('fsr_')||selection.path==='direct'||selection.path==='current')return`${deliveryWidth(selection.output)}×${selection.output}`;
  if(selection.path==='nativeaa')return`${deliveryWidth(selection.output)}×${selection.output}`;
  const scale=Number(selection.multiplier)/200;
  return`${evenDimension(deliveryWidth(selection.output)*scale)}×${evenDimension(Number(selection.output)*scale)}`;
}
function optionField(label,key,selected,values,wide=false,idPrefix='field'){const id=`${idPrefix}-${key}`;return`<div class="field ${wide?'wide':''}"><label for="${id}">${esc(label)}</label><select id="${id}" data-key="${key}">${values.map(([value,text])=>`<option value="${esc(value)}" ${String(value)===String(selected)?'selected':''}>${esc(text)}</option>`).join('')}</select></div>`}
function resolutionField(label,key,selected,values,idPrefix){return optionField(label,key,selected,values.map(value=>[value,`${value}p`]),false,idPrefix)}
function buttonGroup(label,key,values,selected){return`<div class="field wide"><span class="field-label">${esc(label)}</span><div class="state-buttons" data-key="${key}" role="group" aria-label="${esc(label)}">${values.map(([value,text])=>`<button type="button" data-value="${esc(value)}" class="${String(value)===String(selected)?'active':''}" aria-pressed="${String(value)===String(selected)}">${esc(text)}</button>`).join('')}</div></div>`}
function renderPanel(side){
  const item=normalizeSelection(state[side]);const entry=catalogEntry(item);const name=canonicalFilename(item);
  const advanced=item.path==='supersample'?buttonGroup('Reconstruction multiplier','multiplier',MULTIPLIERS,item.multiplier)+buttonGroup('Independent CAS arm','placement',CAS_PLACEMENTS,item.placement):item.path==='nativeaa'?buttonGroup('Independent CAS arm','placement',CAS_PLACEMENTS,item.placement):'';
  const viewModes=item.method.startsWith('fsr_')?VIEW_MODES:[VIEW_MODES[0]];const target=$(`${side}-panel`);
  target.innerHTML=`<header class="panel-head"><h3><span>${side==='left'?'A':'B'}</span>${side==='left'?'Left':'Right'} path</h3><span class="asset-state ${entry?'loaded':''}" data-status>${entry?'Validated asset':'No current asset'}</span></header><div class="selectors">${optionField('Scene','scene',item.scene,SCENES,true,side)}${resolutionField('Input','input',item.input,INPUTS,side)}${resolutionField('Delivery','output',item.output,outputsForInput(item.input),side)}${buttonGroup('Reconstruction path','path',PATHS,item.path)}${advanced}${optionField('Inspection view','view',item.view,viewModes,false,side)}</div><div class="technical-readout"><div>Method <b>${esc(methodLabel(item))}</b></div><div>Route <b>${item.input}p → ${item.output}p</b></div><div>Expected grid <b>${workingGrid(item)}</b></div><div>Loaded pixels <b data-dimensions>awaiting image</b></div><div>CAS <b>${item.method.includes('no_cas')?'absent':item.method.includes('external_post')?'post-reduction':item.method.includes('cas20')?'resolve/integrated':'n/a'}</b></div><div>View <b>${item.view}</b></div><div class="file-row">File <b>${esc(entry?.path||name)}</b></div></div>`;
  target.querySelectorAll('button').forEach(button=>button.addEventListener('click',()=>{item[button.closest('[data-key]').dataset.key]=button.dataset.value;render()}));
  target.querySelectorAll('select').forEach(select=>select.addEventListener('change',()=>{item[select.dataset.key]=['input','output','multiplier'].includes(select.dataset.key)?Number(select.value):select.value;render()}));
}
function loadImage(side){
  const item=state[side],image=$(`${side}-image`),entry=catalogEntry(item),panel=$(`${side}-panel`),status=panel.querySelector('[data-status]'),dimensions=panel.querySelector('[data-dimensions]');let failed=false;
  image.onload=()=>{if(failed)return;dimensions.textContent=`${image.naturalWidth}×${image.naturalHeight}`;status.textContent='Loaded · hash recorded';status.className='asset-state loaded'};
  image.onerror=()=>{failed=true;image.src='images/no_image.svg';dimensions.textContent='unavailable';status.textContent='No current asset';status.className='asset-state'};
  if(!entry||entry.validation!=='validated_experiment'){failed=true;image.src='images/no_image.svg';dimensions.textContent='unavailable';return}image.src=entry.path;
}
function minZoom(){return Math.min(...[state.left.output,state.right.output].map(output=>output>=1440?50:100))}
function applyView(){state.zoom=Math.max(minZoom(),Math.min(300,state.zoom));$('comparison').style.setProperty('--split',`${state.split}%`);const size=Math.max(100,state.zoom);$('comparison-stage').style.width=`${size}%`;$('comparison-stage').style.height=`${size}%`;$('zoom').min=minZoom();$('zoom').value=state.zoom;$('zoom-value').textContent=`${state.zoom}%`}
function render(){renderPanel('left');renderPanel('right');loadImage('left');loadImage('right');applyView();$('left-viewport-label').textContent=`A · ${methodLabel(state.left)}`;$('right-viewport-label').textContent=`B · ${methodLabel(state.right)}`}
function renderMatrix(){$('route-matrix').innerHTML=INPUTS.map(input=>`<article class="route-column"><h3>${input}p <span>input</span></h3><div class="route-list">${outputsForInput(input).map(output=>`<div class="route"><span>${input}p → ${output}p</span><small>${(output/input).toFixed(2)}×</small></div>`).join('')}</div></article>`).join('')}

$('sweep-input').addEventListener('input',event=>{state.split=Number(event.target.value);applyView()});$('zoom').addEventListener('input',event=>{state.zoom=Number(event.target.value);applyView()});$('reset').addEventListener('click',()=>{state.split=50;state.zoom=100;$('sweep-input').value=50;$('comparison').scrollTo(0,0);applyView()});$('swap').addEventListener('click',()=>{[state.left,state.right]=[state.right,state.left];render()});
let dragging=false,lastX=0,lastY=0;$('comparison').addEventListener('pointerdown',event=>{if(event.target===$('sweep-input'))return;dragging=true;lastX=event.clientX;lastY=event.clientY;$('comparison').setPointerCapture(event.pointerId)});$('comparison').addEventListener('pointermove',event=>{if(!dragging)return;$('comparison').scrollLeft-=event.clientX-lastX;$('comparison').scrollTop-=event.clientY-lastY;lastX=event.clientX;lastY=event.clientY});$('comparison').addEventListener('pointerup',()=>{dragging=false});$('comparison').addEventListener('pointercancel',()=>{dragging=false});
window.__tforgeCanonicalFilename=canonicalFilename;const validCount=CATALOG.size;$('asset-count').textContent=validCount?`${validCount} validated catalog assets`:'Awaiting the new campaign capture';$('asset-count').classList.toggle('ready',validCount>0);renderMatrix();render();
