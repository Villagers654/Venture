# Binary repackaging of the baseline release layout (build_packages.sh rpm).
Name:           venture
Version:        @VERSION@
Release:        1
Summary:        Play Meta Quest games on PC VR
License:        GPL-3.0-or-later
URL:            https://venture-emu.org/
ExclusiveArch:  x86_64
# Everything beyond glibc and the desktop session is bundled in /opt/venture;
# automatic dependency/provide scanning would export the bundled libraries.
AutoReqProv:    no
# Library capabilities, not package names: they resolve on Fedora and openSUSE.
Requires:       libc.so.6(GLIBC_2.35)(64bit)
Requires:       libgcc_s.so.1()(64bit)
Recommends:     libvulkan.so.1()(64bit)
Recommends:     libsecret-1.so.0()(64bit)
Recommends:     libfontconfig.so.1()(64bit)
%global debug_package %{nil}
%global __strip /bin/true
%global __brp_check_rpaths %{nil}

%description
Venture runs Meta Quest games on a PC VR headset through OpenXR. Every library
it needs beyond glibc, the Vulkan loader and the desktop session is bundled in
/opt/venture.

%install
cp -a %{stage}/. %{buildroot}/

%files
/opt/venture
/usr/bin/venture
/usr/share/applications/org.venture_emu.Venture.desktop
/usr/share/icons/hicolor/scalable/apps/org.venture_emu.Venture.svg
/usr/share/metainfo/org.venture_emu.Venture.metainfo.xml
