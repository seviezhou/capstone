#!/bin/bash
set -e -x

cd bindings/python
sudo rm /usr/bin/python && sudo ln -s /opt/python/cp27-cp27m/bin/python /usr/bin/python; python -V

# Compile wheels
if [ -f /opt/python/cp36-cp36m/bin/python ];then
  /opt/python/cp36-cp36m/bin/python setup.py bdist_wheel
else
  python3 setup.py bdist_wheel
fi
cd dist
auditwheel repair *.whl
<<<<<<< HEAD
mv -f wheelhouse/*.whl .
=======
mv -f wheelhouse/*.whl .
>>>>>>> 00f5057fad5fbb623c9d7aa4e3e00e499954556e
