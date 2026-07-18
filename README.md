# Reverse engineered DLSS/DLAA implementation for Assetto Corsa in VR
## Works alongside CSP gracefully

Download release from releases section, run install.bat for it to try and detect where your AC folder is and move the required files in place, or itll ask you where it is.

Early build, testing may show issues, but seems ok so far, though because CSP has approximately sixteen million different options its hard to know of any potential bad interactions.


### To make it work:

in Content Manager:

- Enable post-processing in settings->assetto corsa->video

- Disable MSAA in Content Manager  in settings->assetto corsa->video

- Disable Post-process antialiasing in settings->custom shaders patch->graphics adjustments

- Disable Single Pass Stereo in settings->custom shaders patch->VR

- Probably disable VRS too in that same section, might not play well

After install edit acre.ini to change settings, set it to DLSS or DLAA , quality level, sharpness etc

DLSS is not a great benefit due to a higher CPU cost than normal, but it does reduce GPU load a bit, so might be helpful on older cards

DLAA is nice though, very similar results to MSAA 8x but at a much lower performance cost

I will not paywall this on Patreon, this is GPL licensed, its free, use it. have it, fork it, improve it. 

If X4fab implements VR DLAA into CSP then ill probably retire this as his will probably be better. but in the meantime, this works. 
