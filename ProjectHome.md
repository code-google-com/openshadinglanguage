Open Shading Language (OSL) is a small but rich language for
programmable shading in advanced renderers and other applications, ideal
for describing materials, lights, displacement, and pattern generation.

Please read [this introduction](http://code.google.com/p/openshadinglanguage/wiki/OSL_Introduction) for an more detailed overview of some of OSL's unique features, description of the current state of the project, and future road map.  Also you may download the [language specification](https://github.com/imageworks/OpenShadingLanguage/blob/master/src/doc/osl-languagespec.pdf).

The OSL project includes a complete language specification, a compiler from OSL to an intermediate assembly-like bytecode, a runtime library interpreter that executes the shaders (including just-in-time machine code generation using LLVM), and extensive standard shader function library. These all exist as libraries with straightforward C++ APIs, and so may be easily integrated into existing renderers, compositing packages, image processing tools, or other applications. Additionally, the source code can be easily customized to allow for renderer-specific extensions or alterations, or custom back-ends to translate to GPUs or other special hardware.

OSL was developed by Sony Pictures Imageworks for use in its in-house
renderer used for feature film animation and visual effects. The
language specification was developed with input by other visual effects
and animation studios who also wish to use it.


## GitHub ##

We no longer maintain the SVN repository on Google Code.  Instead, you should visit the GitHub project page for OSL:
https://github.com/imageworks/OpenShadingLanguage/


## Join our Google Groups / Email Lists ##

  * Developers (and for now, all OSL topics): http://groups.google.com/group/osl-dev

