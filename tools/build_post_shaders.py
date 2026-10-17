"""Compila os shaders de pós-processamento (runtime/vitart/post/*.comp) e grava post_shaders.h.

    python tools/build_post_shaders.py [caminho do glslc]

O vitart embute o SPIR-V (não precisa de arquivos ao lado do executável). Rode de novo depois de mexer
nos .comp ou no post_common.glsl.
"""
import os
import struct
import subprocess
import sys

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'runtime', 'vitart', 'post')
GLSLC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get('VULKAN_SDK', 'C:/VulkanSDK/latest'), 'Bin', 'glslc.exe')


def main():
    out = ['// Gerado por tools/build_post_shaders.py a partir de runtime/vitart/post/*.comp. Não edite.', '#pragma once',
           '#include <cstdint>', '']
    for name in ('prep', 'blur', 'final'):
        src = os.path.join(HERE, 'post_%s.comp' % name)
        spv = os.path.join(HERE, 'post_%s.spv' % name)
        subprocess.check_call([GLSLC, '--target-env=vulkan1.1', '-O', '-o', spv, src])
        words = struct.unpack('<%dI' % (os.path.getsize(spv) // 4), open(spv, 'rb').read())
        out.append('static const uint32_t kPost_%s[%d] = {' % (name, len(words)))
        for i in range(0, len(words), 8):
            out.append('    ' + ', '.join('0x%08x' % w for w in words[i:i + 8]) + ',')
        out.append('};')
        out.append('')
    with open(os.path.join(HERE, '..', 'post_shaders.h'), 'w', newline='\n') as f:
        f.write('\n'.join(out))
    print('post_shaders.h gravado')


if __name__ == '__main__':
    main()
