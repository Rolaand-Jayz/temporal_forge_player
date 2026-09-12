# Native motion-scale probe

This probe tests the native INT8 path with the opt-in motion-vector scale set
to 0.5x and 2.0x. It uses the same 426x240 real clips and 1920x1080 output as
the other native probes.

Both variants are numerically identical to the native default within capture
noise on Tears of Steel daylight and Sintel cave. That means these flags do
not currently change the native output for this capture path; they are not a
quality candidate to promote. The result is also a useful handoff clue: the
native graph may not consume this prepass motion-scale control, or these clips
may be supplying no motion that reaches that control.
