#!/usr/bin/env
import os, sys

prefix = sys.argv[2]

sys.path.append('/home/philippe/Development/ATIDLAS/build/python/pyatidlas/build/lib.linux-x86_64-2.7/')
sys.path.append(os.path.join(prefix, 'pysrc'))

a = Analysis([os.path.join(prefix, 'pysrc','autotune.py')],
         hiddenimports=['scipy.sparse.csgraph._validation',
                         'scipy.special._ufuncs_cxx',
                         'scipy.sparse.linalg.dsolve.umfpack',
                         'scipy.integrate.vode',
                         'scipy.integrate.lsoda',
                         'sklearn.utils.sparsetools._graph_validation',
                         'sklearn.utils.sparsetools._graph_tools',
                         'sklearn.utils.lgamma',
                         'sklearn.tree._utils'],
         hookspath=None,
         excludes=['scipy.io.matlab','matplotlib','PyQt4'],
         runtime_hooks=None)
pyz = PYZ(a.pure)
exe = EXE(pyz,
      a.scripts,
      a.binaries,
      a.zipfiles,
      a.datas,
      name='autotune',
      debug=False,
      strip=None,
      upx=True,
      console=True )
