Features
========

Ease-of-use Python API
----------------------

Intel® Extension for PyTorch\* provides simple frontend Python APIs and utilities for users to get performance optimizations such as graph optimization and operator optimization with minor code changes. Typically, only two to three clauses are required to be added to the original code.

Please check `API Documentation <api_doc.html>`_ page for details of API functions. Examples are available in `Examples <examples.html>`_ page.

.. note::

   Please check the following table for package name of Intel® Extension for PyTorch\* from version to version when you do the package importing in Python scripts.

   .. list-table::
      :widths: auto
      :align: center
      :header-rows: 1
   
      * - version
        - package name
      * - 1.2.0 ~ 1.9.0
        - intel_pytorch_extension
      * - 1.10.0 ~
        - intel_extension_for_pytorch

Channels Last
-------------

Comparing to the default NCHW memory format, channels_last (NHWC) memory format could further accelerate convolutional neural networks. In Intel® Extension for PyTorch\*, NHWC memory format has been enabled for most key CPU operators, though not all of them have been merged to PyTorch master branch yet. They are expected to be fully landed in PyTorch upstream soon.

Check more detailed information for `Channels Last <features/nhwc.html>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/nhwc

Auto Mixed Precision (AMP)
--------------------------

Low precision data type BFloat16 has been natively supported on the 3rd Generation Xeon®  Scalable Processors (aka Cooper Lake) with AVX512 instruction set and will be supported on the next generation of Intel® Xeon® Scalable Processors with Intel® Advanced Matrix Extensions (Intel® AMX) instruction set with further boosted performance. The support of Auto Mixed Precision (AMP) with BFloat16 for CPU and BFloat16 optimization of operators have been massively enabled in Intel® Extension for PyTorch\*, and partially upstreamed to PyTorch master branch. Most of these optimizations will be landed in PyTorch master through PRs that are being submitted and reviewed.

Check more detailed information for `Auto Mixed Precision (AMP) <features/amp.html>`_.

Bfloat16 computation can be conducted on platforms with AVX512 instruction set. On platforms with `AVX512 BFloat16 instruction <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-deep-learning-boost-new-instruction-bfloat16.html>`_, there will be further performance boost.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/amp

Graph Optimization
------------------

To optimize performance further with torchscript, Intel® Extension for PyTorch\* supports fusion of frequently used operator patterns, like Conv2D+ReLU, Linear+ReLU, etc.  The benefit of the fusions are delivered to users in a transparant fashion.

Check more detailed information for `Graph Optimization <features/graph_optimization.html>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/graph_optimization

Operator Optimization
---------------------

Intel® Extension for PyTorch\* also optimizes operators and implements several customized operators for performance boost. A few  ATen operators are replaced by their optimized counterparts in Intel® Extension for PyTorch\* via ATen registration mechanism. Moreover, some customized operators are implemented for several popular topologies. For instance, ROIAlign and NMS are defined in Mask R-CNN. To improve performance of these topologies, Intel® Extension for PyTorch\* also optimized these customized operators.

.. currentmodule:: intel_extension_for_pytorch.nn
.. autoclass:: FrozenBatchNorm2d

.. currentmodule:: intel_extension_for_pytorch.nn.functional
.. autofunction:: interaction

Optimizer Optimization
----------------------

Optimizers are one of key parts of the training workloads. Intel Extension for PyTorch brings two types of optimizations to optimizers:
1.	Operator fusion for the computation in the optimizers.
2.	SplitSGD for BF16 training, which reduces the memory footprint of the master weights by half.


Check more detailed information for `Split SGD <features/split_sgd.html>`_ and `Optimizer Fusion <features/optimizer_fusion.html>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/split_sgd
   features/optimizer_fusion

Runtime Extension (Experimental)
--------------------------------

Intel® Extension for PyTorch* Runtime Extension provides a couple of PyTorch frontend APIs for users to get finer-grained control of the thread runtime. It provides

1. Multi-stream inference via the Python frontend module MultiStreamModule.
2. Spawn asynchronous tasks from both Python and C++ frontend.
3. Configure core bindings for OpenMP threads from both Python and C++ frontend.

Please **note**: Intel® Extension for PyTorch* Runtime extension is still in the **POC** stage. The API is subject to change. More detailed descriptions are available at `API Documentation page <api_doc.html>`_.

Check more detailed information for `Runtime Extension <features/runtime_extension.html>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/runtime_extension

INT8 Quantization (Experimental)
--------------------------------

Intel® Extension for PyTorch* has built-in quantization recipes to deliver good statistical accuracy for most popular DL workloads including CNN, NLP and recommendation models. The quantized model is then optimized with the `oneDNN graph <https://spec.oneapi.io/onednn-graph/latest/introduction.html>`_ fusion pass to deliver good performance.

Check more detailed information for `INT8 <features/int8.html>`_.

oneDNN provides an evaluation feature called `oneDNN Graph Compiler <https://github.com/oneapi-src/oneDNN/tree/dev-graph-preview4/doc#onednn-graph-compiler>`_. Please refer to `oneDNN build instruction <https://github.com/oneapi-src/oneDNN/blob/dev-graph-preview4/doc/build/build_options.md#build-graph-compiler>`_ to try this feature. You can find oneDNN in `third_party/llga`.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/int8
