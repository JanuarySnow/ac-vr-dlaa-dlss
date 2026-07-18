# Linking Exception

This project links against and redistributes the NVIDIA NGX / DLSS SDK
(`vendor/DLSS`, `redist/nvngx_dlss.dll`) and MinHook (`vendor/minhook`) as
required to enable DLSS support on NVIDIA RTX hardware. The NVIDIA SDK is
licensed under the NVIDIA RTX SDKs License and is not, and cannot be, GPL
licensed.

As a special exception, the copyright holders of this project give you
permission to link this program (or a modified version of it) with the
NVIDIA NGX / DLSS SDK, and to convey the resulting combined work, without
requiring the NVIDIA SDK to be licensed under the terms of the GNU General
Public License. You must still comply with the GPL for every other part of
the combined work, and with the NVIDIA RTX SDKs License for the NVIDIA SDK
components themselves.

This exception applies only to the named proprietary SDK required for
hardware-vendor upscaling support. It does not extend the GPL's permissions
to any other code, and it does not apply to modifications you make to this
project's own source code, which remain fully covered by the GPL.

(Modeled on the GCC Runtime Library Exception and similar linking
exceptions used by other GPL projects to permit linking against
license-incompatible vendor SDKs.)
