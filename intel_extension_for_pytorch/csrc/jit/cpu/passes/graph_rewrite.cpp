
#include "graph_rewrite.h"
#include <torch/csrc/jit/passes/remove_mutation.h>
#include "utils.h"

namespace torch {
namespace jit {
namespace graph_rewrite {

// those code just copy from PyTorch offical:
// https://github.com/pytorch/pytorch/blob/master/torch/csrc/jit/passes/graph_rewrite_helper.cpp

Value* getValue(
    const std::string& name,
    const std::unordered_map<const Value*, Value*>& match_vmap,
    const std::unordered_map<std::string, Value*>& vmap) {
  return match_vmap.at(vmap.at(name));
}

c10::optional<IValue> getIValue(
    const std::string& name,
    const std::unordered_map<const Value*, Value*>& match_vmap,
    const std::unordered_map<std::string, Value*>& vmap) {
  return toIValue(getValue(name, match_vmap, vmap));
}

void FuseShuffle(std::shared_ptr<Graph>& graph) {
  std::string shuffle = R"(
      graph(%input, %view_shape:int[], %trans_dim0:int, %trans_dim1:int, %mem_format:int, %flattern_shape:int[]):
        %r1 = aten::view(%input, %view_shape)
        %r2 = aten::transpose(%r1, %trans_dim0, %trans_dim1)
        %r3 = aten::contiguous(%r2, %mem_format)
        %r4 = aten::view(%r3, %flattern_shape)
        return (%r4) )";

  std::string shuffle_2d_fusion = R"(
      graph(%input, %view_shape:int[], %trans_dim0:int, %trans_dim1:int, %mem_format:int, %flattern_shape:int[]):
        %r = ipex::shuffle_2d(%input, %view_shape, %trans_dim0, %trans_dim1)
        return (%r) )";

  // this filter passes only for the following conditions:
  // (1) the first view is [n, c, h, w] => [n, groups, c // groups, h, w]
  // (2) the tranpose is for groups => [n, c // groups, grpups, h, w]
  // (3) the output view shape should be the same as the input tensor shape
  auto filter_shuffle_2d_fusion =
      [](const Match& match,
         const std::unordered_map<std::string, Value*>& vmap) {
        const auto& match_vmap = match.values_map;

        // current node is the second "view" node
        Node* node = match.anchor;
        // get the first "view" node
        auto first_view_node =
            node->input(0)->node()->input(0)->node()->input(0)->node();
        // get the input tensor from the first "view" node
        auto input_ = first_view_node->input(0);
        auto inputType = input_->type()->cast<TensorType>();
        auto inputTensor = *inputType;
        // if the input tensor does not have dim info
        if (!inputType->dim().has_value()) {
          return false;
        }
        // get the view shape
        auto view_shape_ = first_view_node->input(1);
        // get the flattern shape
        auto flattern_shape_ = node->input(1);
        // get the transpose node
        auto trans_node = node->input(0)->node()->input(0)->node();

        auto trans_dim0_ = trans_node->input(1);
        auto trans_dim1_ = trans_node->input(2);
        // if the transpose dim has not set
        if (!toIValue(trans_dim0_).has_value() ||
            !toIValue(trans_dim1_).has_value()) {
          return false;
        }
        auto trans_dim0_val = toIValue(trans_dim0_).value().toInt();
        auto trans_dim1_val = toIValue(trans_dim1_).value().toInt();
        auto dim0_val =
            trans_dim0_val < trans_dim1_val ? trans_dim0_val : trans_dim1_val;
        auto dim1_val =
            trans_dim0_val > trans_dim1_val ? trans_dim0_val : trans_dim1_val;
        // If the tranpose if not for groups. ex. [n, c1, c2, h, w] => [n, c2,
        // c1, h, w]
        if ((dim1_val - dim0_val) != 1) {
          return false;
        }

        // if the view shape and flattern shape is not set
        if (!toIValue(view_shape_).has_value() ||
            !toIValue(flattern_shape_).has_value()) {
          return false;
        }
        auto view_shape_list = toIValue(view_shape_).value().toIntVector();
        auto flattern_shape_list =
            toIValue(flattern_shape_).value().toIntVector();

        // ex. [n, c, h, w] => [n, groups, c // groups, h, w]
        if ((inputType->dim().value() - view_shape_list.size()) != -1) {
          return false;
        }

        TORCH_INTERNAL_ASSERT_DEBUG_ONLY(dim0_val >= 0);
        TORCH_INTERNAL_ASSERT_DEBUG_ONLY(dim1_val >= 0);
        TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
            dim0_val + 1 < inputType->dim().value());
        TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
            dim1_val + 1 < inputType->dim().value());

        auto view_shape_dim0_val = view_shape_list[dim0_val];
        auto view_shape_dim1_val = view_shape_list[dim1_val];
        // c => groups, c // groups
        if (view_shape_dim0_val * view_shape_dim1_val !=
            inputTensor.sizes()[dim0_val].value()) {
          return false;
        }
        // output view shape should be the same as the input
        if (flattern_shape_list.size() != inputType->dim().value()) {
          return false;
        }

        for (int i = 0; i < flattern_shape_list.size(); i++) {
          if (flattern_shape_list[i] != inputTensor.sizes()[i].value()) {
            // [n, c, h, w] => view [n, groups, c // groups, h, w] => tranpose
            // [n, c // groups, groups, h, w]
            // => view [n, -1, h, w]
            //    or
            //    view [n, c, h, w]
            if ((flattern_shape_list[i] != -1) || (i != dim0_val)) {
              return false;
            }
          }
        }
        return true;
      };

  SubgraphRewriter rewriter_shuffle_2d;
  rewriter_shuffle_2d.RegisterRewritePattern(shuffle, shuffle_2d_fusion);
  rewriter_shuffle_2d.runOnGraph(graph, filter_shuffle_2d_fusion);
}

void FuseAddLayerNorm(std::shared_ptr<Graph>& graph) {
  std::string aten_add_layernorm = R"(
      graph(%add_a, %add_b, %alpha, %shape:int[], %w, %b, %eps:float, %cudnn_enable:bool):
        %s = aten::add(%add_a, %add_b, %alpha)
        %r = aten::layer_norm(%s, %shape, %w, %b, %eps, %cudnn_enable)
        return (%r) )";
  std::string fused_add_layernorm = R"(
      graph(%add_a, %add_b, %alpha, %shape:int[], %w, %b, %eps:float, %cudnn_enable:bool):
        %r = ipex::add_layernorm(%add_a, %add_b, %alpha, %shape, %w, %b, %eps, %cudnn_enable)
        return (%r) )";
  SubgraphRewriter rewriter_aten;
  rewriter_aten.RegisterRewritePattern(aten_add_layernorm, fused_add_layernorm);
  rewriter_aten.runOnGraph(graph);
}

// MHA fusion covers aten::softmax, ipex::softmax and ipex::softmax_:
// (1) MHA obviously shows better performance than aten div/matmul/add/softmax.
// (2) MHA also shows better performance than aten add + matmul_div fusion
//     + ipex::softmax/softmax_.
// (3) Current ipex::softmax/softmax_ is from the replacement of aten::softmax,
//     it is safe to make MHA cover ipex::softmax/softmax_.
void FuseMHAScoreCalc(std::shared_ptr<Graph>& graph) {
  // below are basic patterns for MHA matching
  std::string div_matmul_add = R"(
      graph(%q: Tensor, %k: Tensor, %relative_qk: Tensor, %alpha:int, %dim_per_head:int, %softmax_dim:int, %dtype):
        %_q = aten::div(%q, %dim_per_head)
        %qk = aten::matmul(%_q, %k)
        %_scores = aten::add(%qk, %relative_qk, %alpha) )";

  std::string matmul_div_add = R"(
      graph(%q: Tensor, %k: Tensor, %relative_qk: Tensor, %alpha:int, %dim_per_head:int, %softmax_dim:int, %dtype):
        %qk = aten::matmul(%q, %k)
        %_qk = aten::div(%qk, %dim_per_head)
        %_scores = aten::add(%_qk, %relative_qk, %alpha) )";

  std::string div_matmul_expand = R"(
      graph(%q: Tensor, %k: Tensor, %mask_qk: Tensor, %mask_qk_reshp: int[], %transpose_dim_a:int, %transpose_dim_b:int, %fill:float, %dim_per_head:float, %softmax_dim:int, %dtype):
        %_q = aten::div(%q, %dim_per_head)
        %_k = aten::transpose(%k, %transpose_dim_a, %transpose_dim_b)
        %qk = aten::matmul(%_q, %_k)
        %_mask_qk_view = aten::view(%mask_qk, %mask_qk_reshp)
        %_mask_qk_shape = aten::expand_as(%_mask_qk_view, %qk) )";

  std::string aten_masked_fill = R"(
        %_scores = aten::masked_fill(%qk, %_mask_qk_shape, %fill) )";

  std::string aten_masked_fill_ = R"(
        %_scores = aten::masked_fill_(%qk, %_mask_qk_shape, %fill) )";

  std::string aten_softmax = R"(
        %scores = aten::softmax(%_scores, %softmax_dim, %dtype) )";

  std::string set_return = R"(
        return (%scores) )";

  auto filter_distil_mha =
      [](const Match& match,
         const std::unordered_map<std::string, Value*>& vmap) {
        const auto& match_vmap = match.values_map;

        // Only support last dimension for softmax
        auto dim_ = getIValue("softmax_dim", match_vmap, vmap).value();
        if (!(dim_.isInt())) {
          return false;
        }
        auto dim = dim_.toInt();
        if (dim != -1) {
          return false;
        }

        Node* node = match.anchor;
        // Find the masked_fill node to get the qk value
        auto qk_node = node->input(0)->node();
        TORCH_CHECK(
            qk_node->kind() == aten::masked_fill ||
            qk_node->kind() == aten::masked_fill_);
        // Find the view node to get the mask value
        auto mask_node =
            node->input(0)->node()->input(1)->node()->input(0)->node();
        TORCH_CHECK(mask_node->kind() == aten::view);
        auto mask_value = mask_node->input(0)->type()->cast<TensorType>();
        // Only support contiguous tensor for qk and mask
        auto qk_value = qk_node->input(0)->type()->cast<TensorType>();
        auto qk_value_contiguous = qk_value->contiguous();
        auto mask_value_contiguous = mask_value->contiguous();
        bool is_contiguous_qk =
            qk_value_contiguous->strides() == qk_value->strides();
        bool is_contiguous_mask =
            mask_value_contiguous->strides() == mask_value->strides();
        if (!is_contiguous_qk || !is_contiguous_mask) {
          return false;
        }

        // Only support qk.dim >=2D
        bool not_one_dim = qk_value->dim().value() >= 2;
        if (!not_one_dim) {
          return false;
        }

        // Only support 64byte aligned
        auto qk_tensor = *qk_value;
        bool aligned_64_bytes =
            qk_tensor.sizes()[qk_value->dim().value() - 1].value() % 16 == 0;
        if (!aligned_64_bytes) {
          return false;
        }

        // Only support when expand from the mid dims shape (bs :: seq_length)
        auto mask_reshape_node = mask_node->input(1)->node();
        for (int i = 1; i < qk_value->dim().value() - 1; i++) {
          auto expand_check =
              toIValue(mask_reshape_node->inputs().at(i)).value().toInt();
          if (!(expand_check == 1)) {
            return false;
          }
        }

        // Checking the dtype as None
        auto dtype_value = getIValue("dtype", match_vmap, vmap).value();
        if (!dtype_value.isNone()) {
          return false;
        }

        return true;
      };

  std::string div_matmul_add_softmax_fusion = R"(
      graph(%q: Tensor, %k: Tensor, %relative_qk: Tensor, %alpha:int, %dim_per_head:int, %softmax_dim:int, %dtype):
        %scores = ipex::mha_scores_calc(%q, %k, %relative_qk, %alpha, %dim_per_head, %softmax_dim, %dtype)
        return (%scores) )";

  std::string div_matmul_maskedfill_softmax_fusion = R"(
      graph(%q: Tensor, %k: Tensor, %mask_qk: Tensor, %mask_qk_reshp: int[], %transpose_dim_a:int, %transpose_dim_b:int, %fill:float, %dim_per_head:float, %softmax_dim:int, %dtype):
        %scores = ipex::distil_mha_scores_calc(%q, %k, %mask_qk, %mask_qk_reshp, %transpose_dim_a, %transpose_dim_b, %fill, %dim_per_head, %softmax_dim, %dtype)
        return (%scores) )";

  SubgraphRewriter mha_fusion;
  SubgraphRewriter distil_mha_fusion;

  // below are MHA combinations for Bert Model (div+matmul+add+softmax)
  std::string div_matmul_add_softmax =
      div_matmul_add + aten_softmax + set_return;
  std::string matmul_div_add_softmax =
      matmul_div_add + aten_softmax + set_return;
  mha_fusion.RegisterRewritePattern(
      div_matmul_add_softmax, div_matmul_add_softmax_fusion);
  mha_fusion.RegisterRewritePattern(
      matmul_div_add_softmax, div_matmul_add_softmax_fusion);
  // below are MHA combinations for DistilBert Model
  // (div+matmul+masked_fill+softmax)
  std::string div_matmul_maskfill__softmax =
      div_matmul_expand + aten_masked_fill_ + aten_softmax + set_return;
  std::string div_matmul_maskfill_softmax =
      div_matmul_expand + aten_masked_fill + aten_softmax + set_return;
  distil_mha_fusion.RegisterRewritePattern(
      div_matmul_maskfill__softmax, div_matmul_maskedfill_softmax_fusion);
  distil_mha_fusion.RegisterRewritePattern(
      div_matmul_maskfill_softmax, div_matmul_maskedfill_softmax_fusion);

  mha_fusion.runOnGraph(graph);
  distil_mha_fusion.runOnGraph(graph, filter_distil_mha);
}

void replaceAtenMaxPool2dWithIpexMaxPool2d(std::shared_ptr<Graph>& graph) {
  std::string max_pool2d = R"(
      graph(%a, %kernel_size:int[], %stride:int[], %padding:int[], %dilation:int[], %ceil_mode:bool):
        %r = aten::max_pool2d(%a, %kernel_size, %stride, %padding, %dilation, %ceil_mode)
        return (%r) )";
  std::string ipex_max_pool2d = R"(
      graph(%a, %kernel_size:int[], %stride:int[], %padding:int[], %dilation:int[], %ceil_mode:bool):
        %r = ipex::max_pool2d(%a, %kernel_size, %stride, %padding, %dilation, %ceil_mode)
        return (%r) )";
  SubgraphRewriter rewriter_max_pool2d;
  rewriter_max_pool2d.RegisterRewritePattern(max_pool2d, ipex_max_pool2d);
  rewriter_max_pool2d.runOnGraph(graph);
}

// ipex::softmax shows better performance than aten::softmax, but compared with
// ipex::softmax_, it is slower.
// Like ipex::softmax_, we only do the replacement when the input
// is contiguous.
void replaceAtenSoftmaxWithIpexSoftmax(std::shared_ptr<Graph>& graph) {
  std::string aten_softmax = R"(
      graph(%a, %dim:int, %half_to_float:bool):
        %r = aten::softmax(%a, %dim, %half_to_float)
        return (%r) )";
  std::string ipex_softmax = R"(
      graph(%a, %dim:int, %half_to_float:bool):
        %r = ipex::softmax(%a, %dim, %half_to_float)
        return (%r) )";

  // Filter the unsupported case for inplace softmax
  auto filter_outplace =
      [](const Match& match,
         const std::unordered_map<std::string, Value*>& vmap) {
        Node* node = match.anchor;
        // check if the input is contiguous, and skip if it is not
        auto input_value = node->input(0)->type()->cast<TensorType>();
        if (!is_contiguous(input_value)) {
          return false;
        }

        return true;
      };

  SubgraphRewriter rewriter_aten;
  rewriter_aten.RegisterRewritePattern(aten_softmax, ipex_softmax);
  rewriter_aten.runOnGraph(graph, filter_outplace);
}

void replaceAtenBatchNormWithIpexBatchNorm(std::shared_ptr<Graph>& graph) {
  std::string batch_norm = R"(
      graph(%a, %weight, %bias, %running_mean, %running_var, %training, %momentum, %eps, %cudnn_enabled):
        %r = aten::batch_norm(%a, %weight, %bias, %running_mean, %running_var, %training, %momentum, %eps, %cudnn_enabled)
        return (%r) )";
  std::string ipex_batch_norm = R"(
      graph(%a, %weight, %bias, %running_mean, %running_var, %training, %momentum, %eps, %cudnn_enabled):
        %r = ipex::batch_norm(%a, %weight, %bias, %running_mean, %running_var, %training, %momentum, %eps, %cudnn_enabled)
        return (%r) )";

  SubgraphRewriter rewriter_batch_norm;
  rewriter_batch_norm.RegisterRewritePattern(batch_norm, ipex_batch_norm);
  rewriter_batch_norm.runOnGraph(graph);
}

void replaceEmbeddingBagWithQEmbeddingBag(std::shared_ptr<Graph>& graph) {
  std::string qembedingbag = R"(
     graph(%weight, %input, %offsets, %sparse, %include_last_offset, %o_scale, %o_zp, %o_dtype):
        %r = ipex::qembedding_bag(%weight, %input, %offsets, %sparse, %include_last_offset, %o_scale, %o_zp, %o_dtype)
        return (%r) )";

  std::string embeddingbag_with_quant_dequant = R"(
      graph(%qweight, %input, %offsets, %sparse, %include_last_offset,  %o_scale, %o_zp, %o_dtype):
        %dqw = aten::dequantize(%qweight)
        %r = torch_ipex::embedding_bag(%dqw, %input, %offsets, %sparse, %include_last_offset)
        %qout = aten::quantize_per_tensor(%r, %o_scale, %o_zp, %o_dtype)
        return (%qout) )";

  SubgraphRewriter rewriter_qembeddingbag;
  rewriter_qembeddingbag.RegisterRewritePattern(
      embeddingbag_with_quant_dequant, qembedingbag);
  rewriter_qembeddingbag.runOnGraph(graph);
}

void replaceInteractionWithQInteraction(std::shared_ptr<Graph>& graph) {
  std::vector<std::string> patterns;
  std::vector<std::string> replacements;
  std::string graph_common_head = R"(graph()";
  std::string graph_common_tail = R"(, %o_scale, %o_zp, %o_dtype):
  )";
  std::string list_construct_common_head =
      R"(%input : Tensor[] = prim::ListConstruct()";
  std::string list_construct_common_tail = R"() )";
  std::string replacement_common_tail =
      R"(%out =  ipex::qinteraction(%input, %o_scale, %o_zp, %o_dtype) return (%out) )";
  std::string pattern_common_tail =
      R"(%out = torch_ipex::interaction_forward(%input)  %qout = aten::quantize_per_tensor(%out, %o_scale, %o_zp, %o_dtype) return (%qout) )";

  for (auto* n : graph->block()->nodes()) {
    if (n->kind() ==
        Symbol::fromQualString("torch_ipex::interaction_forward")) {
      size_t id = 0;
      auto ListConstructNode = n->input(0)->node();

      bool is_quantized = std::any_of(
          ListConstructNode->inputs().begin(),
          ListConstructNode->inputs().end(),
          [](auto& v) {
            return v->node()->kind() == Symbol::aten("dequantize");
          });

      if (!is_quantized)
        return;

      std::string pattern = R"()";
      std::string replacement = R"()";
      std::string dequantizes = R"()";
      std::vector<std::string> qinputs;
      std::vector<std::string> dqinputs;
      for (auto input : ListConstructNode->inputs()) {
        if (input->node()->kind() == Symbol::aten("dequantize")) {
          qinputs.push_back("%q" + std::to_string(id));
          dqinputs.push_back("%dq" + std::to_string(id));
          std::string dequantize = "%dq" + std::to_string(id) +
              " : Tensor = aten::dequantize(" + "%q" + std::to_string(id) + ")";
          dequantizes.append(dequantize);
          ++id;
        }
      }

      std::string header =
          graph_common_head + c10::Join(", ", qinputs) + graph_common_tail;
      pattern += header;
      pattern += dequantizes;
      pattern += list_construct_common_head + c10::Join(", ", dqinputs) +
          list_construct_common_tail;
      pattern += pattern_common_tail;
      patterns.push_back(pattern);

      replacement = header;
      replacement += list_construct_common_head + c10::Join(", ", qinputs) +
          list_construct_common_tail;
      replacement += replacement_common_tail;
      replacements.push_back(replacement);
    }
  }

  SubgraphRewriter rewriter;
  for (size_t i = 0; i < patterns.size(); i++) {
    rewriter.RegisterRewritePattern(patterns[i], replacements[i]);
    rewriter.runOnGraph(graph);
  }
}

void fuseBmmAdd(std::shared_ptr<Graph>& graph) {
  std::array<std::string, 2> add_operators = {"add", "add_"};

  auto bmm_add_rstring_v1 = R"(
    graph(%input, %batch1, %batch2, %alpha):
        %x = aten::bmm(%batch1, %batch2)
        %res = aten::add(%x, %input, %alpha)
        return (%res))";
  std::string bmm_add_fused = R"(
    graph(%input, %batch1, %batch2, %alpha):
        %res = ipex::bmm_add(%input, %batch1, %batch2, %alpha)
        return (%res))";
  // fliter the unsupported case
  auto fusion_filter = [](const Match& match,
                          const std::unordered_map<std::string, Value*>& vmap) {
    const auto& match_vmap = match.values_map;

    auto batch1 =
        getValue("batch1", match_vmap, vmap)->type()->cast<TensorType>();
    auto batch2 =
        getValue("batch2", match_vmap, vmap)->type()->cast<TensorType>();
    if (batch1->dim() != batch2->dim()) {
      return false;
    }

    if (batch1->dim().value() < 3) {
      return false;
    }

    return true;
  };

  SubgraphRewriter rewriter_add_v1;
  rewriter_add_v1.RegisterRewritePattern(bmm_add_rstring_v1, bmm_add_fused);
  rewriter_add_v1.runOnGraph(graph, fusion_filter);
}

void FuseConcatBnRelu(std::shared_ptr<Graph>& graph) {
  std::string aten_concat_bn_relu = R"(
      graph(%input : Tensor[], %dim:int, %weight, %bias, %running_mean, %running_var, %training, %momentum, %eps, %cudnn_enabled):
        %a = aten::cat(%input, %dim)
        %b = aten::batch_norm(%a, %weight, %bias, %running_mean, %running_var, %training, %momentum, %eps, %cudnn_enabled)
        %c = aten::relu(%b)
        return (%c) )";
  std::string fused_concat_bn_relu = R"(
      graph(%input : Tensor[], %dim:int, %weight, %bias, %running_mean, %running_var, %training, %momentum, %eps, %cudnn_enabled):
        %alpha: int = prim::Constant[value=1]()
        %u1 = aten::add(%running_var, %eps, %alpha)
        %u2 = aten::sqrt(%u1)
        %scale = aten::div(%weight, %u2)
        %u3 = aten::mul(%running_mean, %scale)
        %beta = aten::sub(%bias, %u3, %alpha)
        %b = ipex::concat_bn_relu(%input, %scale, %beta, %weight, %bias, %running_mean, %running_var, %training, %momentum, %eps, %cudnn_enabled, %dim)
        return (%b) )";

  auto fusion_filter = [](const Match& match,
                          const std::unordered_map<std::string, Value*>& vmap) {
    Node* node = match.anchor;
    const auto& match_vmap = match.values_map;
    // Check if the Concat Dimension is the channel
    auto dim_ = getIValue("dim", match_vmap, vmap).value();
    if (!(dim_.isInt())) {
      return false;
    }
    auto dim = dim_.toInt();
    if (dim != 1) {
      return false;
    }
    // Find the Concat node
    auto n = node->input(0)->node()->input(0)->node();
    TORCH_CHECK(n->kind() == aten::cat);

    auto listConstruct = n->input(0)->node();
    int64_t list_length = 0;
    for (auto p : listConstruct->inputs()) {
      list_length++;
    }
    // Check if the Concat list is not empty
    TORCH_CHECK(list_length);

    auto tensor1 = listConstruct->input(0)->type()->cast<TensorType>();
    auto check_type_channelsize = [](c10::TensorType tensor) {
      return (
          (tensor.scalarType().value() == at::kFloat ||
           tensor.scalarType().value() == at::kBFloat16) &&
          tensor.sizes()[1].value() % 16 == 0 && is_channelslast(tensor));
    };
    // Check if the dimension of the first tensor is either 4 or 5.
    // Check if the data type, the size of Channels, and the memory format are
    // float, mutiples of 16, and ChannelsLast(3d), respectively.
    if (!(tensor1->dim().value() == 4 || tensor1->dim().value() == 5) ||
        !check_type_channelsize(*tensor1)) {
      return false;
    }
    // Check the rest tensors
    for (int64_t i = 1; i < list_length; ++i) {
      auto tensori = listConstruct->input(i)->type()->cast<TensorType>();
      // Check dimension, data type, channel size and memory format
      if (!(tensor1->dim().value() == tensori->dim().value()) ||
          !check_type_channelsize(*tensori)) {
        return false;
      }
      // The channel sizes can be different, and check the other dim sizes.
      for (int64_t j = 0; j < tensori->dim().value(); ++j) {
        if (j != 1 &&
            tensor1->sizes()[j].value() != tensori->sizes()[j].value()) {
          return false;
        }
      }
    }
    // Check if the BN weights is fp32 datatype.
    auto bn_node = node->input(0)->node();
    if (bn_node->namedInput("weight")
            ->type()
            ->cast<TensorType>()
            ->scalarType()
            .value() != at::kFloat) {
      return false;
    }
    return true;
  };

  SubgraphRewriter rewriter_concatbnrelu;
  rewriter_concatbnrelu.RegisterRewritePattern(
      aten_concat_bn_relu, fused_concat_bn_relu);
  rewriter_concatbnrelu.runOnGraph(graph, fusion_filter);
}

void FuseLinearSwishCustomized(std::shared_ptr<Graph>& graph) {
  std::string linear_swish = R"(
      graph(%x, %weight, %bias):
        %_linear_res = aten::linear(%x, %weight, %bias)
        %_sigmod_res = aten::sigmoid(%_linear_res)
        %_mul_res2 = aten::mul(%_linear_res, %_sigmod_res)
        return (%_mul_res2) )";

  std::string linear_swish_fusion = R"(
      graph(%x, %weight, %bias):
        %_res = ipex::linear_swish_customized(%x, %weight, %bias)
        return (%_res) )";

  SubgraphRewriter ls_fusion;
  ls_fusion.RegisterRewritePattern(linear_swish, linear_swish_fusion);
  ls_fusion.runOnGraph(graph);
}

} // namespace graph_rewrite
} // namespace jit
} // namespace torch
