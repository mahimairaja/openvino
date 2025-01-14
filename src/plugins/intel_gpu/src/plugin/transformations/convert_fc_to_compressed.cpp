// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "convert_fc_to_compressed.hpp"
#include <memory>

#include "intel_gpu/op/fully_connected.hpp"
#include "intel_gpu/op/fully_connected_compressed.hpp"

#include "openvino/op/constant.hpp"
#include "openvino/op/subtract.hpp"
#include "openvino/op/matmul.hpp"
#include "openvino/op/convert.hpp"
#include "openvino/op/transpose.hpp"
#include "openvino/op/reshape.hpp"
#include "openvino/core/rt_info.hpp"
#include "openvino/pass/pattern/op/pattern.hpp"
#include "openvino/pass/pattern/op/wrap_type.hpp"
#include "openvino/pass/pattern/op/or.hpp"
#include "transformations/utils/utils.hpp"

namespace ov {
namespace intel_gpu {

ConvertFullyConnectedToFullyConnectedCompressed::ConvertFullyConnectedToFullyConnectedCompressed() {
    using namespace ov::pass::pattern;

    auto compressed_constant = [](const ov::Output<ov::Node>& output) {
        return (output.get_element_type() == ov::element::u8 ||
                output.get_element_type() == ov::element::i8) &&
               output.get_target_inputs().size() == 1;
    };

    auto reshape_3d_to_2d = [](const ov::Output<ov::Node>& output) {
        auto in_ps = output.get_node()->get_input_partial_shape(0);
        auto out_ps = output.get_node()->get_output_partial_shape(0);
        return in_ps.rank().is_static() && out_ps.rank().is_static() && in_ps.size() == 3 && out_ps.size() == 2;
    };

    auto weights_m = wrap_type<ov::op::v0::Constant>(compressed_constant);
    auto convert_m = wrap_type<ov::op::v0::Convert>({weights_m});

    auto sub_const_m = wrap_type<ov::op::v0::Constant>(consumers_count(1));
    auto subtract_m = wrap_type<ov::op::v1::Subtract>({convert_m, sub_const_m});

    auto mul_const_m = wrap_type<ov::op::v0::Constant>(consumers_count(1));
    auto mul_with_sub_m = wrap_type<ov::op::v1::Multiply>({subtract_m, mul_const_m});
    auto mul_no_sub_m = wrap_type<ov::op::v1::Multiply>({convert_m, mul_const_m});
    auto mul_m = std::make_shared<ov::pass::pattern::op::Or>(OutputVector{mul_with_sub_m, mul_no_sub_m});

    auto reshape_const_m = wrap_type<ov::op::v0::Constant>();
    auto reshape_m = wrap_type<ov::op::v1::Reshape>({mul_m, reshape_const_m}, reshape_3d_to_2d);

    auto transpose_input = std::make_shared<ov::pass::pattern::op::Or>(OutputVector{reshape_m, mul_m});
    auto transpose_const_m = wrap_type<ov::op::v0::Constant>();
    auto transpose_m = wrap_type<ov::op::v1::Transpose>({transpose_input, transpose_const_m});

    auto data_m = any_input();
    auto weights_input_m = std::make_shared<ov::pass::pattern::op::Or>(ov::OutputVector{reshape_m, transpose_m, mul_m});
    auto fully_connected_m = wrap_type<op::FullyConnected>({data_m, weights_input_m});

    ov::matcher_pass_callback callback = [=](ov::pass::pattern::Matcher& m) {
        const auto& pattern_map = m.get_pattern_value_map();
        OPENVINO_ASSERT(pattern_map.count(fully_connected_m));
        OPENVINO_ASSERT(pattern_map.count(mul_const_m));
        OPENVINO_ASSERT(pattern_map.count(weights_m));
        OPENVINO_ASSERT(pattern_map.count(convert_m));
        auto fc = std::dynamic_pointer_cast<op::FullyConnected>(pattern_map.at(fully_connected_m).get_node_shared_ptr());
        if (!fc || transformation_callback(fc)) {
            return false;
        }

        bool has_transpose = pattern_map.count(transpose_m);
        auto scale_shape = pattern_map.at(mul_const_m).get_shape();
        bool grouped = std::count_if(scale_shape.begin(), scale_shape.end(), [](size_t d) { return d > 1; }) > 1;

        auto reshape_const_to_2d = [has_transpose, grouped](std::shared_ptr<ov::Node> node) {
            auto constant = std::dynamic_pointer_cast<ov::op::v0::Constant>(node);
            OPENVINO_ASSERT(constant != nullptr);
            ov::Shape current_shape = constant->get_shape();
            if (current_shape.size() == 2)
                return constant;
            OPENVINO_ASSERT(current_shape.size() == 3);

            auto new_shape = (has_transpose || !grouped) ? ov::Shape{current_shape[0] * current_shape[1], current_shape[2]}
                                                         : ov::Shape{current_shape[0], current_shape[1] * current_shape[2]};

            return std::make_shared<ov::op::v0::Constant>(*constant, new_shape);
        };

        const auto& fc_input_a = fc->get_input_node_shared_ptr(0);
        const auto& scale = reshape_const_to_2d(pattern_map.at(mul_const_m).get_node_shared_ptr());
        std::shared_ptr<ov::Node> optional_zero_point = nullptr;

        const bool with_zero_point = pattern_map.count(subtract_m) > 0;
        if (with_zero_point) {
            optional_zero_point = reshape_const_to_2d(pattern_map.at(sub_const_m).get_node_shared_ptr());
        }

        std::shared_ptr<ov::Node> fc_input_b = reshape_const_to_2d(pattern_map.at(weights_m).get_node_shared_ptr());
        std::shared_ptr<ov::Node> fc_input_scale = scale;
        std::shared_ptr<ov::Node> fc_input_zp = optional_zero_point;
        if (has_transpose) {
            const auto& transpose = pattern_map.at(transpose_m).get_node_shared_ptr();
            std::shared_ptr<ov::Node> transpose_const = pattern_map.at(transpose_const_m).get_node_shared_ptr();
            if (ov::shape_size(transpose_const->get_shape()) != fc_input_b->get_output_partial_shape(0).size()) {
                std::vector<int32_t> new_order(fc_input_b->get_output_partial_shape(0).size());
                std::iota(new_order.begin(), new_order.end(), 0);
                std::swap(new_order[new_order.size() - 1], new_order[new_order.size() - 2]);
                transpose_const = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{new_order.size()}, new_order);
            }

            fc_input_b = transpose->clone_with_new_inputs({ fc_input_b->output(0), transpose_const });
            fc_input_scale = transpose->clone_with_new_inputs({ scale->output(0), transpose_const });
            if (with_zero_point)
                fc_input_zp = transpose->clone_with_new_inputs({ optional_zero_point->output(0), transpose_const });
        }

        std::shared_ptr<ov::Node> new_fc = nullptr;
        if (with_zero_point) {
            new_fc = std::make_shared<op::FullyConnectedCompressed>(fc_input_a,
                                                                    fc_input_b,
                                                                    fc_input_scale,
                                                                    fc_input_zp,
                                                                    fc->get_output_type());
        } else {
            new_fc = std::make_shared<op::FullyConnectedCompressed>(fc_input_a,
                                                                    fc_input_b,
                                                                    fc_input_scale,
                                                                    fc->get_output_type());
        }

        new_fc->set_friendly_name(fc->get_friendly_name());
        ov::copy_runtime_info(m.get_matched_nodes(), new_fc);
        ov::replace_node(fc, new_fc);
        return true;
    };

    auto m = std::make_shared<ov::pass::pattern::Matcher>(fully_connected_m, "ConvertFullyConnectedToFullyConnectedCompressed");
    this->register_matcher(m, callback);
}

}  // namespace intel_gpu
}  // namespace ov
