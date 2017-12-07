#!/bin/sh
echo "/* DO NOT CHANGE HEADER FILE BY HAND! CHANGE THE extract-shell.sh */"
echo "/* SCRIPT THIS IS GENERATED.  ADD A CHANGELOG ENTRY IF YOU MODIFY */"
echo "/* THIS SCRIPT.                                                   */"
echo "/* ALWAYS ADD A CHANGELOG OR I WILL PERSONALLY KICK YOUR ASS! */"
grep gettextfunc | fgrep -v 'gettextfunc ()' | sed 's/^.*gettextfunc[^"]*\("[^"]*"\).*$/const char *foo = N_(\1);/'
