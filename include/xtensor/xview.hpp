/***************************************************************************
* Copyright (c) 2016, Johan Mabille, Sylvain Corlay and Wolf Vollprecht    *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#ifndef XTENSOR_VIEW_HPP
#define XTENSOR_VIEW_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

#include <xtl/xclosure.hpp>
#include <xtl/xsequence.hpp>
#include <xtl/xmeta_utils.hpp>
#include <xtl/xtype_traits.hpp>

#include "xbroadcast.hpp"
#include "xcontainer.hpp"
#include "xiterable.hpp"
#include "xsemantic.hpp"
#include "xslice.hpp"
#include "xtensor_forward.hpp"
#include "xtensor.hpp"
#include "xview_utils.hpp"


namespace xt
{

    /*********************
     * xview declaration *
     *********************/

    template <class CT, class... S>
    struct xcontainer_inner_types<xview<CT, S...>>
    {
        using xexpression_type = std::decay_t<CT>;
        using temporary_type = view_temporary_type_t<xexpression_type, S...>;
    };

    template <bool is_const, class CT, class... S>
    class xview_stepper;

    template <class ST, class... S>
    struct xview_shape_type;

    namespace detail
    {

        template <class T>
        struct is_xrange
            : std::false_type
        {
        };

        template <class T>
        struct is_xrange<xrange<T>>
            : std::true_type
        {
        };

        template <class S>
        struct is_xall_slice
            : std::false_type
        {
        };

        template <class T>
        struct is_xall_slice<xall<T>>
            : std::true_type
        {
        };

        template <layout_type L, bool valid, bool all_seen, bool range_seen, class V>
        struct is_contiguous_view_impl
        {
            static constexpr bool value = false;
        };

        template <class T>
        struct static_dimension
        {
            static constexpr std::ptrdiff_t value = -1;
        };

        template <class T, std::size_t N>
        struct static_dimension<std::array<T, N>>
        {
            static constexpr std::ptrdiff_t value = static_cast<std::ptrdiff_t>(N);
        };

        template <class T, std::size_t N>
        struct static_dimension<xt::const_array<T, N>>
        {
            static constexpr std::ptrdiff_t value = static_cast<std::ptrdiff_t>(N);
        };

        template <std::size_t... I>
        struct static_dimension<xt::fixed_shape<I...>>
        {
            static constexpr std::ptrdiff_t value = sizeof...(I);
        };

        // if we have the same number of integers as we have static dimensions
        // this can be interpreted like a xscalar
        template <class CT, class... S>
        struct is_xscalar_impl<xview<CT, S...>>
        {
            static constexpr bool value = static_cast<std::ptrdiff_t>(integral_count<S...>()) == static_dimension<typename std::decay_t<CT>::shape_type>::value ? true : false;
        };

        template <class S>
        struct is_strided_slice_impl
            : std::true_type
        {
        };

        template <class T>
        struct is_strided_slice_impl<xkeep_slice<T>>
            : std::false_type
        {
        };

        template <class T>
        struct is_strided_slice_impl<xdrop_slice<T>>
            : std::false_type
        {
        };

        // If we have no discontiguous slices, we can calculate strides for this view.
        template <class E, class... S>
        struct is_strided_view
            : std::integral_constant<bool, xtl::conjunction<has_data_interface<E>, is_strided_slice_impl<S>...>::value>
        {
        };

        // if row major the view can only be (statically) computed as contiguous if:
        // any number of integers is followed by either one or no range which 
        // are followed by explicit (or implicit) all's
        // 
        // e.g. 
        //      (i, j, all(), all()) == contiguous
        //      (i, range(0, 2), all()) == contiguous
        //      (i) == contiguous (implicit all slices)
        //      (i, all(), j) == *not* contiguous
        //      (i, range(0, 2), range(0, 2)) == *not* contiguous etc.
        template <bool valid, bool all_seen, bool range_seen, class V>
        struct is_contiguous_view_impl<layout_type::row_major, valid, all_seen, range_seen, V>
        {
            using slice = xtl::mpl::front_t<V>;
            static constexpr bool is_range_slice = is_xrange<slice>::value;
            static constexpr bool is_int_slice = std::is_integral<slice>::value;
            static constexpr bool is_all_slice = is_xall_slice<slice>::value;
            static constexpr bool have_all_seen = all_seen || is_all_slice;
            static constexpr bool have_range_seen = is_range_slice;

            static constexpr bool is_valid = valid && (have_all_seen ? is_all_slice : (!range_seen && (is_int_slice || is_range_slice)));

            static constexpr bool value = is_contiguous_view_impl<layout_type::row_major,
                                                                  is_valid,
                                                                  have_all_seen,
                                                                  range_seen || is_range_slice,
                                                                  xtl::mpl::pop_front_t<V>>::value;
        };

        template <bool valid, bool all_seen, bool range_seen>
        struct is_contiguous_view_impl<layout_type::row_major, valid, all_seen, range_seen, xtl::mpl::vector<>>
        {
            static constexpr bool value = valid;
        };

        // For column major the *same* but reverse is true -- with the additional
        // constraint that we have to know the dimension at compile time otherwise
        // we cannot make the decision as there might be implicit all's following.
        template <bool valid, bool int_seen, bool range_seen, class V>
        struct is_contiguous_view_impl<layout_type::column_major, valid, int_seen, range_seen, V>
        {
            using slice = xtl::mpl::front_t<V>;
            static constexpr bool is_range_slice = is_xrange<slice>::value;
            static constexpr bool is_int_slice = std::is_integral<slice>::value;
            static constexpr bool is_all_slice = is_xall_slice<slice>::value;

            static constexpr bool have_int_seen = int_seen || is_int_slice;

            static constexpr bool is_valid = valid && (have_int_seen ? is_int_slice : (!range_seen && (is_all_slice || is_range_slice)));
            static constexpr bool value = is_contiguous_view_impl<layout_type::column_major,
                                                                  is_valid,
                                                                  have_int_seen,
                                                                  is_range_slice || range_seen,
                                                                  xtl::mpl::pop_front_t<V>>::value;
        };

        template <bool valid, bool int_seen, bool range_seen>
        struct is_contiguous_view_impl<layout_type::column_major, valid, int_seen, range_seen, xtl::mpl::vector<>>
        {
            static constexpr bool value = valid;
        };

        // TODO relax has_data_interface constraint here!
        template <class E, class... S>
        struct is_contiguous_view
            : std::integral_constant<bool, 
                has_data_interface<E>::value &&
                !(E::static_layout == layout_type::column_major && static_dimension<typename E::shape_type>::value != sizeof...(S)) &&
                is_contiguous_view_impl<E::static_layout, true, false, false, xtl::mpl::vector<S...>>::value
              >
        {
        };

        template <layout_type L, class T, std::ptrdiff_t offset>
        struct unwrap_offset_container
        {
            using type = void;
        };

        template <class T, std::ptrdiff_t offset>
        struct unwrap_offset_container<layout_type::row_major, T, offset>
        {
            using type = sequence_view<T, offset, static_dimension<T>::value>;
        };

        template <class T, std::ptrdiff_t start, std::ptrdiff_t end, std::ptrdiff_t offset>
        struct unwrap_offset_container<layout_type::row_major, sequence_view<T, start, end>, offset>
        {
            using type = sequence_view<T, start + offset, end>;
        };

        template <class T, std::ptrdiff_t offset>
        struct unwrap_offset_container<layout_type::column_major, T, offset>
        {
            using type = sequence_view<T, 0, static_dimension<T>::value - offset>;
        };

        template <class T, std::ptrdiff_t start, std::ptrdiff_t end, std::ptrdiff_t offset>
        struct unwrap_offset_container<layout_type::column_major, sequence_view<T, start, end>, offset>
        {
            using type = sequence_view<T, start, end - offset>;
        };

        template <class E, class... S>
        struct get_contigous_shape_type
        {
            // if we have no `range` in the slices we can re-use the shape with an offset
            using type = std::conditional_t<xtl::disjunction<is_xrange<S>...>::value, 
                                            typename xview_shape_type<typename E::shape_type, S...>::type,
                                            // In the false branch we know that we have only integers at the front OR end, and NO range
                                            typename unwrap_offset_container<E::static_layout, typename E::inner_shape_type, integral_count<S...>()>::type>;
        };

        template <class T>
        struct is_sequence_view
            : std::integral_constant<bool, false>
        {
        };

        template <class T, std::ptrdiff_t S, std::ptrdiff_t E>
        struct is_sequence_view<sequence_view<T, S, E>>
            : std::integral_constant<bool, true>
        {
        };
    }

    template <class CT, class... S>
    struct xiterable_inner_types<xview<CT, S...>>
    {
        using xexpression_type = std::decay_t<CT>;

        static constexpr bool is_strided_view = detail::is_strided_view<xexpression_type, S...>::value;
        static constexpr bool is_contiguous_view = detail::is_contiguous_view<xexpression_type, S...>::value;


        using inner_shape_type = std::conditional_t<is_contiguous_view, 
                                                    typename detail::get_contigous_shape_type<xexpression_type, S...>::type,
                                                    typename xview_shape_type<typename xexpression_type::shape_type, S...>::type>;

        using stepper = std::conditional_t<is_strided_view,
                                           xstepper<xview<CT, S...>>,
                                           xview_stepper<std::is_const<std::remove_reference_t<CT>>::value, CT, S...>>;

        using const_stepper = std::conditional_t<is_strided_view,
                                                 xstepper<const xview<CT, S...>>,
                                                 xview_stepper<true, std::remove_cv_t<CT>, S...>>;
    };

    /**
     * @class xview
     * @brief Multidimensional view with tensor semantic.
     *
     * The xview class implements a multidimensional view with tensor
     * semantic. It is used to adapt the shape of an xexpression without
     * changing it. xview is not meant to be used directly, but
     * only with the \ref view helper functions.
     *
     * @tparam CT the closure type of the \ref xexpression to adapt
     * @tparam S the slices type describing the shape adaptation
     *
     * @sa view, range, all, newaxis, keep, drop
     */
    template <class CT, class... S>
    class xview : public xview_semantic<xview<CT, S...>>,
                  public xiterable<xview<CT, S...>>
    {
    public:

        using self_type = xview<CT, S...>;
        using xexpression_type = std::decay_t<CT>;
        using semantic_base = xview_semantic<self_type>;
        using temporary_type = typename xcontainer_inner_types<self_type>::temporary_type;

        static constexpr bool is_const = std::is_const<std::remove_reference_t<CT>>::value;
        using value_type = typename xexpression_type::value_type;
        using simd_value_type = xsimd::simd_type<value_type>;
        using reference = std::conditional_t<is_const,
                                             typename xexpression_type::const_reference,
                                             typename xexpression_type::reference>;
        using const_reference = typename xexpression_type::const_reference;
        using pointer = std::conditional_t<is_const,
                                           typename xexpression_type::const_pointer,
                                           typename xexpression_type::pointer>;
        using const_pointer = typename xexpression_type::const_pointer;
        using size_type = typename xexpression_type::size_type;
        using difference_type = typename xexpression_type::difference_type;

        static constexpr layout_type static_layout = detail::is_contiguous_view<xexpression_type, S...>::value ?
                                                                                xexpression_type::static_layout :
                                                                                layout_type::dynamic;
        static constexpr bool contiguous_layout = static_layout != layout_type::dynamic;

        static constexpr bool is_strided_view = detail::is_strided_view<xexpression_type, S...>::value;
        static constexpr bool is_contiguous_view = contiguous_layout;

        using iterable_base = xiterable<self_type>;
        using inner_shape_type = typename iterable_base::inner_shape_type;
        using shape_type = typename xview_shape_type<typename xexpression_type::shape_type, S...>::type;

        using xexpression_inner_strides_type = xtl::mpl::eval_if_t<has_strides<xexpression_type>,
                                                                   detail::expr_inner_strides_type<xexpression_type>,
                                                                   get_strides_type<shape_type>>;

        using storage_type = xtl::mpl::eval_if_t<has_data_interface<xexpression_type>,
                                                 detail::expr_storage_type<xexpression_type>,
                                                 make_invalid_type<>>;

        using inner_strides_type = std::conditional_t<is_contiguous_view, 
                                                      typename detail::unwrap_offset_container<xexpression_type::static_layout, xexpression_inner_strides_type, integral_count<S...>()>::type,
                                                      get_strides_t<shape_type>>;

        using inner_backstrides_type = inner_strides_type;
        using strides_type = get_strides_t<shape_type>;
        using back_strides_type = strides_type;


        using slice_type = std::tuple<S...>;

        using stepper = typename iterable_base::stepper;
        using const_stepper = typename iterable_base::const_stepper;

        using storage_iterator = std::conditional_t<has_data_interface<xexpression_type>::value,
                                                    typename xexpression_type::storage_iterator,
                                                    typename iterable_base::storage_iterator>;
        using const_storage_iterator = std::conditional_t<has_data_interface<xexpression_type>::value,
                                                    typename xexpression_type::const_storage_iterator,
                                                    typename iterable_base::const_storage_iterator>;

        using container_iterator = pointer;
        using const_container_iterator = const_pointer;

        // The FSL argument prevents the compiler from calling this constructor
        // instead of the copy constructor when sizeof...(SL) == 0.
        template <class CTA, class FSL, class... SL>
        explicit xview(CTA&& e, FSL&& first_slice, SL&&... slices) noexcept;

        xview(const xview&) = default;
        self_type& operator=(const xview& rhs);

        template <class E>
        self_type& operator=(const xexpression<E>& e);

        template <class E>
        disable_xexpression<E, self_type>& operator=(const E& e);

        size_type dimension() const noexcept;

        size_type size() const noexcept;
        const inner_shape_type& shape() const noexcept;
        const slice_type& slices() const noexcept;
        layout_type layout() const noexcept;

        template <class T>
        void fill(const T& value);

        template <class... Args>
        reference operator()(Args... args);
        template <class... Args>
        reference at(Args... args);
        template <class... Args>
        reference unchecked(Args... args);
        template <class OS>
        disable_integral_t<OS, reference> operator[](const OS& index);
        template <class I>
        reference operator[](std::initializer_list<I> index);
        reference operator[](size_type i);
        template <class It>
        reference element(It first, It last);

        template <class... Args>
        const_reference operator()(Args... args) const;
        template <class... Args>
        const_reference at(Args... args) const;
        template <class... Args>
        const_reference unchecked(Args... args) const;
        template <class OS>
        disable_integral_t<OS, const_reference> operator[](const OS& index) const;
        template <class I>
        const_reference operator[](std::initializer_list<I> index) const;
        const_reference operator[](size_type i) const;
        template <class It>
        const_reference element(It first, It last) const;

        template <class ST>
        bool broadcast_shape(ST& shape, bool reuse_cache = false) const;

        template <class ST>
        bool is_trivial_broadcast(const ST& strides) const;

        template <class ST, bool Enable = is_strided_view>
        std::enable_if_t<!Enable, stepper>
        stepper_begin(const ST& shape);
        template <class ST, bool Enable = is_strided_view>
        std::enable_if_t<!Enable, stepper>
        stepper_end(const ST& shape, layout_type l);

        template <class ST, bool Enable = is_strided_view>
        std::enable_if_t<!Enable, const_stepper>
        stepper_begin(const ST& shape) const;
        template <class ST, bool Enable = is_strided_view>
        std::enable_if_t<!Enable, const_stepper>
        stepper_end(const ST& shape, layout_type l) const;

        template <class ST, bool Enable = is_strided_view>
        std::enable_if_t<Enable, stepper>
        stepper_begin(const ST& shape);
        template <class ST, bool Enable = is_strided_view>
        std::enable_if_t<Enable, stepper>
        stepper_end(const ST& shape, layout_type l);

        template <class ST, bool Enable = is_strided_view>
        std::enable_if_t<Enable, const_stepper>
        stepper_begin(const ST& shape) const;
        template <class ST, bool Enable = is_strided_view>
        std::enable_if_t<Enable, const_stepper>
        stepper_end(const ST& shape, layout_type l) const;

        template <class T = xexpression_type>
        std::enable_if_t<has_data_interface<T>::value, typename T::storage_type&>
        storage();

        template <class T = xexpression_type>
        std::enable_if_t<has_data_interface<T>::value, const typename T::storage_type&>
        storage() const;

        template <class T = xexpression_type>
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, storage_iterator>
        storage_begin();

        template <class T = xexpression_type>
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, storage_iterator>
        storage_end();

        template <class T = xexpression_type>
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, const_storage_iterator>
        storage_cbegin() const;
        
        template <class T = xexpression_type>
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, const_storage_iterator>
        storage_cend() const;

        template <class T = xexpression_type>
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, const inner_strides_type&>
        strides() const;

        template <class T = xexpression_type>
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, const inner_strides_type&>
        backstrides() const;

        template <class T = xexpression_type>
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, const_pointer>
        data() const;

        template <class T = xexpression_type>
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, pointer>
        data();

        template <class T = xexpression_type>
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, std::size_t>
        data_offset() const noexcept;

        template <class It>
        inline It data_xbegin_impl(It begin) const noexcept;

        template <class It>
        inline It data_xend_impl(It begin, layout_type l) const noexcept;
        inline container_iterator data_xbegin() noexcept;
        inline const_container_iterator data_xbegin() const noexcept;
        inline container_iterator data_xend(layout_type l) noexcept;

        inline const_container_iterator data_xend(layout_type l) const noexcept;

        // Conversion operator enabled for statically "scalar" views
        template <class ST = self_type, class = std::enable_if_t<is_xscalar<std::decay_t<ST>>::value, int>>
        operator reference()
        {
            return (*this)();
        }

        template <class ST = self_type, class = std::enable_if_t<is_xscalar<std::decay_t<ST>>::value, int>>
        operator const_reference() const
        {
            return (*this)();
        }

        size_type underlying_size(size_type dim) const;

        xtl::xclosure_pointer<self_type&> operator&() &;
        xtl::xclosure_pointer<const self_type&> operator&() const &;
        xtl::xclosure_pointer<self_type> operator&() &&;

        template <class E, class T = xexpression_type,
                  class = std::enable_if_t<has_data_interface<T>::value && is_contiguous_view, int>>
        void assign_to(xexpression<E>& e, bool force_resize) const;

        //
        // SIMD interface
        //

        template <class simd>
        using simd_return_type = xsimd::simd_return_type<value_type, typename simd::value_type>;

        template <class T, class R>
        using enable_simd_interface = std::enable_if_t<has_simd_interface<T>::value && is_strided_view, R>;

        template <class align, class simd = simd_value_type, class T = xexpression_type>
        enable_simd_interface<T, void> store_simd(size_type i, const simd& e);

        template <class align, class simd = simd_value_type, class T = xexpression_type>
        enable_simd_interface<T, simd_return_type<simd>> load_simd(size_type i) const;

        template <class T = xexpression_type>
        enable_simd_interface<T, reference> data_element(size_type i);

        template <class T = xexpression_type>
        enable_simd_interface<T, const_reference> data_element(size_type i) const;

    private:

        // VS 2015 workaround (yes, really)
        template <std::size_t I>
        struct lesser_condition
        {
            static constexpr bool value = (I + newaxis_count_before<S...>(I + 1) < sizeof...(S));
        };

        CT m_e;
        slice_type m_slices;
        inner_shape_type m_shape;
        mutable inner_strides_type m_strides;
        mutable inner_backstrides_type m_backstrides;
        mutable std::size_t m_data_offset;
        mutable bool m_strides_computed;

        template <class CTA, class FSL, class... SL>
        explicit xview(std::true_type, CTA&& e, FSL&& first_slice, SL&&... slices) noexcept;

        template <class CTA, class FSL, class... SL>
        explicit xview(std::false_type, CTA&& e, FSL&& first_slice, SL&&... slices) noexcept;

        template <class... Args>
        auto make_index_sequence(Args... args) const noexcept;

        void compute_strides(std::true_type) const;
        void compute_strides(std::false_type) const;

        reference access();

        template <class Arg, class... Args>
        reference access(Arg arg, Args... args);

        const_reference access() const;

        template <class Arg, class... Args>
        const_reference access(Arg arg, Args... args) const;

        template <typename std::decay_t<CT>::size_type... I, class... Args>
        reference unchecked_impl(std::index_sequence<I...>, Args... args);

        template <typename std::decay_t<CT>::size_type... I, class... Args>
        const_reference unchecked_impl(std::index_sequence<I...>, Args... args) const;

        template <typename std::decay_t<CT>::size_type... I, class... Args>
        reference access_impl(std::index_sequence<I...>, Args... args);

        template <typename std::decay_t<CT>::size_type... I, class... Args>
        const_reference access_impl(std::index_sequence<I...>, Args... args) const;

        template <typename std::decay_t<CT>::size_type I, class... Args>
        std::enable_if_t<lesser_condition<I>::value, size_type> index(Args... args) const;

        template <typename std::decay_t<CT>::size_type I, class... Args>
        std::enable_if_t<!lesser_condition<I>::value, size_type> index(Args... args) const;

        template <typename std::decay_t<CT>::size_type, class T>
        size_type sliced_access(const xslice<T>& slice) const;

        template <typename std::decay_t<CT>::size_type I, class T, class Arg, class... Args>
        size_type sliced_access(const xslice<T>& slice, Arg arg, Args... args) const;

        template <typename std::decay_t<CT>::size_type I, class T, class... Args>
        disable_xslice<T, size_type> sliced_access(const T& squeeze, Args...) const;

        using base_index_type = xindex_type_t<typename xexpression_type::shape_type>;

        template <class It>
        base_index_type make_index(It first, It last) const;

        void assign_temporary_impl(temporary_type&& tmp);

        template <std::size_t... I>
        inline std::size_t data_offset_impl(std::index_sequence<I...>) const noexcept;

        template <std::size_t... I>
        inline auto compute_strides_impl(std::index_sequence<I...>) const noexcept;

        auto compute_shape(std::true_type) const
        {
            return inner_shape_type(m_e.shape());
        }

        auto compute_shape(std::false_type) const
        {
            std::size_t dim = m_e.dimension() - integral_count<S...>() + newaxis_count<S...>();
            auto shape = xtl::make_sequence<inner_shape_type>(dim, 0);
            auto func = [](const auto& s) noexcept { return get_size(s); };
            for (size_type i = 0; i != dim; ++i)
            {
                size_type index = integral_skip<S...>(i);
                shape[i] = index < sizeof...(S) ?
                    apply<size_type>(index, func, m_slices) : m_e.shape()[index - newaxis_count_before<S...>(index)];
            }
            return shape;
        }

        friend class xview_semantic<xview<CT, S...>>;
    };

    template <class E, class... S>
    auto view(E&& e, S&&... slices);

    /*****************************
     * xview_stepper declaration *
     *****************************/

    namespace detail
    {
        template <class V>
        struct get_stepper_impl
        {
            using xexpression_type = typename V::xexpression_type;
            using type = typename xexpression_type::stepper;
        };

        template <class V>
        struct get_stepper_impl<const V>
        {
            using xexpression_type = typename V::xexpression_type;
            using type = typename xexpression_type::const_stepper;
        };
    }

    template <class V>
    using get_stepper = typename detail::get_stepper_impl<V>::type;

    template <bool is_const, class CT, class... S>
    class xview_stepper
    {
    public:

        using view_type = std::conditional_t<is_const,
                                             const xview<CT, S...>,
                                             xview<CT, S...>>;
        using substepper_type = get_stepper<view_type>;

        using value_type = typename substepper_type::value_type;
        using reference = typename substepper_type::reference;
        using pointer = typename substepper_type::pointer;
        using difference_type = typename substepper_type::difference_type;
        using size_type = typename view_type::size_type;

        using shape_type = typename substepper_type::shape_type;

        xview_stepper() = default;
        xview_stepper(view_type* view, substepper_type it,
                      size_type offset, bool end = false, layout_type l = XTENSOR_DEFAULT_LAYOUT);

        reference operator*() const;

        void step(size_type dim);
        void step_back(size_type dim);
        void step(size_type dim, size_type n);
        void step_back(size_type dim, size_type n);
        void reset(size_type dim);
        void reset_back(size_type dim);

        void to_begin();
        void to_end(layout_type l);

    private:

        bool is_newaxis_slice(size_type index) const noexcept;
        void to_end_impl(layout_type l);

        template <class F>
        void common_step_forward(size_type dim, F f);
        template <class F>
        void common_step_backward(size_type dim, F f);

        template <class F>
        void common_step_forward(size_type dim, size_type n, F f);
        template <class F>
        void common_step_backward(size_type dim, size_type n, F f);

        template <class F>
        void common_reset(size_type dim, F f, bool backwards);

        view_type* p_view;
        substepper_type m_it;
        size_type m_offset;
        std::array<std::size_t, sizeof...(S)> m_index_keeper;
    };

    // meta-function returning the shape type for an xview
    template <class ST, class... S>
    struct xview_shape_type
    {
        using type = ST;
    };

    template <class I, std::size_t L, class... S>
    struct xview_shape_type<std::array<I, L>, S...>
    {
        using type = std::array<I, L - integral_count<S...>() + newaxis_count<S...>()>;
    };

    template <std::size_t... I, class... S>
    struct xview_shape_type<fixed_shape<I...>, S...>
    {
        using type = typename xview_shape_type<std::array<std::size_t, sizeof...(I)>>::type;
    };

    /************************
     * xview implementation *
     ************************/

    /**
     * @name Constructor
     */

    //@{
    /**
     * Constructs a view on the specified xexpression.
     * Users should not call directly this constructor but
     * use the view function instead.
     * @param e the xexpression to adapt
     * @param first_slice the first slice describing the view
     * @param slices the slices list describing the view
     * @sa view
     */
    template <class CT, class... S>
    template <class CTA, class FSL, class... SL>
    xview<CT, S...>::xview(CTA&& e, FSL&& first_slice, SL&&... slices) noexcept
        : xview(
            std::integral_constant<bool, is_contiguous_view>{},
            std::forward<CTA>(e),
            std::forward<FSL>(first_slice),
            std::forward<SL>(slices)...
          )
    {
    }

    // contigous initializer
    template <class CT, class... S>
    template <class CTA, class FSL, class... SL>
    xview<CT, S...>::xview(std::true_type, CTA&& e, FSL&& first_slice, SL&&... slices) noexcept
        : m_e(std::forward<CTA>(e)),
          m_slices(std::forward<FSL>(first_slice), std::forward<SL>(slices)...),
          m_shape(compute_shape(detail::is_sequence_view<inner_shape_type>{})),
          m_strides(m_e.strides()),
          m_backstrides(m_e.backstrides()),
          m_data_offset(data_offset_impl(std::make_index_sequence<sizeof...(S)>())),
          m_strides_computed(true)
    {
    }

    template <class CT, class... S>
    template <class CTA, class FSL, class... SL>
    xview<CT, S...>::xview(std::false_type, CTA&& e, FSL&& first_slice, SL&&... slices) noexcept
        : m_e(std::forward<CTA>(e)),
          m_slices(std::forward<FSL>(first_slice), std::forward<SL>(slices)...),
          m_shape(compute_shape(std::false_type{})),
          m_strides_computed(false)
    {
    }
    //@}

    template <class CT, class... S>
    inline auto xview<CT, S...>::operator=(const xview& rhs) -> self_type&
    {
        temporary_type tmp(rhs);
        return this->assign_temporary(std::move(tmp));
    }

    /**
     * @name Extended copy semantic
     */
    //@{
    /**
     * The extended assignment operator.
     */
    template <class CT, class... S>
    template <class E>
    inline auto xview<CT, S...>::operator=(const xexpression<E>& e) -> self_type&
    {
        return semantic_base::operator=(e);
    }
    //@}

    template <class CT, class... S>
    template <class E>
    inline auto xview<CT, S...>::operator=(const E& e) -> disable_xexpression<E, self_type>&
    {
        this->fill(e);
        return *this;
    }

    /**
     * @name Size and shape
     */
    //@{
    /**
     * Returns the size of the expression.
     */
    template <class CT, class... S>
    inline auto xview<CT, S...>::size() const noexcept -> size_type
    {
        return compute_size(shape());
    }

    /**
     * Returns the number of dimensions of the view.
     */
    template <class CT, class... S>
    inline auto xview<CT, S...>::dimension() const noexcept -> size_type
    {
        return m_shape.size();
    }

    /**
     * Returns the shape of the view.
     */
    template <class CT, class... S>
    inline auto xview<CT, S...>::shape() const noexcept -> const inner_shape_type&
    {
        return m_shape;
    }

    /**
     * Returns the slices of the view.
     */
    template <class CT, class... S>
    inline auto xview<CT, S...>::slices() const noexcept -> const slice_type&
    {
        return m_slices;
    }

    /**
     * Returns the slices of the view.
     */
    template <class CT, class... S>
    inline layout_type xview<CT, S...>::layout() const noexcept
    {
        return xtl::mpl::static_if<is_strided_view>([&](auto self)
        {
            if (static_layout != layout_type::dynamic)
            {
                return static_layout;
            }
            else
            {
                bool strides_match = do_strides_match(self(this)->shape(), self(this)->strides(), self(this)->m_e.layout());
                return strides_match ? self(this)->m_e.layout() : layout_type::dynamic;
            }
        }, 
        /* else */ [&](auto /*self*/)
        {
            return layout_type::dynamic;
        });
    }
    //@}

    /**
     * @name Data
     */
    //@{

    /**
     * Fills the view with the given value.
     * @param value the value to fill the view with.
     */
    template <class CT, class... S>
    template <class T>
    inline void xview<CT, S...>::fill(const T& value)
    {
        if (layout() != layout_type::dynamic)
        {
            std::fill(this->storage_begin(), this->storage_end(), value);
        }
        else
        {
            std::fill(this->begin(), this->end(), value);
        }
    }

    /**
     * Returns a reference to the element at the specified position in the view.
     * @param args a list of indices specifying the position in the view. Indices
     * must be unsigned integers, the number of indices should be equal or greater
     * than the number of dimensions of the view.
     */
    template <class CT, class... S>
    template <class... Args>
    inline auto xview<CT, S...>::operator()(Args... args) -> reference
    {
        XTENSOR_TRY(check_index(shape(), args...));
        XTENSOR_CHECK_DIMENSION(shape(), args...);
        // The static cast prevents the compiler from instantiating the template methods with signed integers,
        // leading to warning about signed/unsigned conversions in the deeper layers of the access methods
        return access(static_cast<size_type>(args)...);
    }

    /**
     * Returns a reference to the element at the specified position in the expression,
     * after dimension and bounds checking.
     * @param args a list of indices specifying the position in the function. Indices
     * must be unsigned integers, the number of indices should be equal to the number of dimensions
     * of the expression.
     * @exception std::out_of_range if the number of argument is greater than the number of dimensions
     * or if indices are out of bounds.
     */
    template <class CT, class... S>
    template <class... Args>
    inline auto xview<CT, S...>::at(Args... args) -> reference
    {
        check_access(shape(), static_cast<size_type>(args)...);
        return this->operator()(args...);
    }

    /**
     * Returns a reference to the element at the specified position in the view.
     * @param args a list of indices specifying the position in the view. Indices
     * must be unsigned integers, the number of indices must be equal to the number of
     * dimensions of the view, else the behavior is undefined.
     *
     * @warning This method is meant for performance, for expressions with a dynamic
     * number of dimensions (i.e. not known at compile time). Since it may have
     * undefined behavior (see parameters), operator() should be prefered whenever
     * it is possible.
     * @warning This method is NOT compatible with broadcasting, meaning the following
     * code has undefined behavior:
     * \code{.cpp}
     * xt::xarray<double> a = {{0, 1}, {2, 3}};
     * xt::xarray<double> b = {0, 1};
     * auto fd = a + b;
     * double res = fd.unchecked(0, 1);
     * \endcode
     */
    template <class CT, class... S>
    template <class... Args>
    inline auto xview<CT, S...>::unchecked(Args... args) -> reference
    {
        return unchecked_impl(make_index_sequence(args...), static_cast<size_type>(args)...);
    }

    /**
     * Returns a reference to the element at the specified position in the view.
     * @param index a sequence of indices specifying the position in the view. Indices
     * must be unsigned integers, the number of indices in the list should be equal or greater
     * than the number of dimensions of the view.
     */
    template <class CT, class... S>
    template <class OS>
    inline auto xview<CT, S...>::operator[](const OS& index)
        -> disable_integral_t<OS, reference>
    {
        return element(index.cbegin(), index.cend());
    }

    template <class CT, class... S>
    template <class I>
    inline auto xview<CT, S...>::operator[](std::initializer_list<I> index)
        -> reference
    {
        return element(index.begin(), index.end());
    }

    template <class CT, class... S>
    inline auto xview<CT, S...>::operator[](size_type i) -> reference
    {
        return operator()(i);
    }

    template <class CT, class... S>
    template <class It>
    inline auto xview<CT, S...>::element(It first, It last) -> reference
    {
        XTENSOR_TRY(check_element_index(shape(), first, last));
        // TODO: avoid memory allocation
        auto index = make_index(first, last);
        return m_e.element(index.cbegin(), index.cend());
    }

    /**
     * Returns a constant reference to the element at the specified position in the view.
     * @param args a list of indices specifying the position in the view. Indices must be
     * unsigned integers, the number of indices should be equal or greater than the number
     * of dimensions of the view.
     */
    template <class CT, class... S>
    template <class... Args>
    inline auto xview<CT, S...>::operator()(Args... args) const -> const_reference
    {
        XTENSOR_TRY(check_index(shape(), args...));
        XTENSOR_CHECK_DIMENSION(shape(), args...);
        // The static cast prevents the compiler from instantiating the template methods with signed integers,
        // leading to warning about signed/unsigned conversions in the deeper layers of the access methods
        return access(static_cast<size_type>(args)...);
    }

    /**
     * Returns a constant reference to the element at the specified position in the view,
     * after dimension and bounds checking.
     * @param args a list of indices specifying the position in the function. Indices
     * must be unsigned integers, the number of indices should be equal to the number of dimensions
     * of the expression.
     * @exception std::out_of_range if the number of argument is greater than the number of dimensions
     * or if indices are out of bounds.
     */
    template <class CT, class... S>
    template <class... Args>
    inline auto xview<CT, S...>::at(Args... args) const -> const_reference
    {
        check_access(shape(), static_cast<size_type>(args)...);
        return this->operator()(args...);
    }

    /**
     * Returns a constant reference to the element at the specified position in the view.
     * @param args a list of indices specifying the position in the view. Indices
     * must be unsigned integers, the number of indices must be equal to the number of
     * dimensions of the view, else the behavior is undefined.
     *
     * @warning This method is meant for performance, for expressions with a dynamic
     * number of dimensions (i.e. not known at compile time). Since it may have
     * undefined behavior (see parameters), operator() should be prefered whenever
     * it is possible.
     * @warning This method is NOT compatible with broadcasting, meaning the following
     * code has undefined behavior:
     * \code{.cpp}
     * xt::xarray<double> a = {{0, 1}, {2, 3}};
     * xt::xarray<double> b = {0, 1};
     * auto fd = a + b;
     * double res = fd.unchecked(0, 1);
     * \endcode
     */
    template <class CT, class... S>
    template <class... Args>
    inline auto xview<CT, S...>::unchecked(Args... args) const -> const_reference
    {
        return unchecked_impl(make_index_sequence(args...), static_cast<size_type>(args)...);
    }

    /**
     * Returns a constant reference to the element at the specified position in the view.
     * @param index a sequence of indices specifying the position in the view. Indices
     * must be unsigned integers, the number of indices in the list should be equal or greater
     * than the number of dimensions of the view.
     */
    template <class CT, class... S>
    template <class OS>
    inline auto xview<CT, S...>::operator[](const OS& index) const
        -> disable_integral_t<OS, const_reference>
    {
        return element(index.cbegin(), index.cend());
    }

    template <class CT, class... S>
    template <class I>
    inline auto xview<CT, S...>::operator[](std::initializer_list<I> index) const
        -> const_reference
    {
        return element(index.begin(), index.end());
    }

    template <class CT, class... S>
    inline auto xview<CT, S...>::operator[](size_type i) const -> const_reference
    {
        return operator()(i);
    }

    template <class CT, class... S>
    template <class It>
    inline auto xview<CT, S...>::element(It first, It last) const -> const_reference
    {
        // TODO: avoid memory allocation
        auto index = make_index(first, last);
        return m_e.element(index.cbegin(), index.cend());
    }

    /**
     * Returns the data holder of the underlying container (only if the view is on a realized
     * container). ``xt::eval`` will make sure that the underlying xexpression is
     * on a realized container.
     */
    template <class CT, class... S>
    template <class T>
    inline auto xview<CT, S...>::storage() ->
        std::enable_if_t<has_data_interface<T>::value, typename T::storage_type&>
    {
        return m_e.storage();
    }

    template <class CT, class... S>
    template <class T>
    inline auto xview<CT, S...>::storage() const ->
        std::enable_if_t<has_data_interface<T>::value, const typename T::storage_type&>
    {
        return m_e.storage();
    }

    template <class CT, class... S>
    template <class T>
    auto xview<CT, S...>::storage_begin()
        -> std::enable_if_t<has_data_interface<T>::value && is_strided_view, storage_iterator>
    {
        return m_e.storage_begin() + data_offset();
    }

    template <class CT, class... S>
    template <class T>
    auto xview<CT, S...>::storage_end()
        -> std::enable_if_t<has_data_interface<T>::value && is_strided_view, storage_iterator>
    {
        return m_e.storage_begin() + data_offset() + size();
    }

    template <class CT, class... S>
    template <class T>
    auto xview<CT, S...>::storage_cbegin() const
        -> std::enable_if_t<has_data_interface<T>::value && is_strided_view, const_storage_iterator>
    {
        return m_e.storage_cbegin() + data_offset();
    }

    template <class CT, class... S>
    template <class T>
    auto xview<CT, S...>::storage_cend() const
        -> std::enable_if_t<has_data_interface<T>::value && is_strided_view, const_storage_iterator>
    {
        return m_e.storage_cbegin() + data_offset() + size();
    }

    /**
     * Return the strides for the underlying container of the view.
     */
    template <class CT, class... S>
    template <class T>
    inline auto xview<CT, S...>::strides() const ->
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, const inner_strides_type&>
    {
        if (!m_strides_computed)
        {
            compute_strides(std::integral_constant<bool, is_contiguous_view>{});
            m_strides_computed = true;
        }
        return m_strides;
    }

    template <class CT, class... S>
    template <class T>
    inline auto xview<CT, S...>::backstrides() const ->
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, const inner_strides_type&>
    {
        if (!m_strides_computed)
        {
            compute_strides(std::integral_constant<bool, is_contiguous_view>{});
            m_strides_computed = true;
        }
        return m_backstrides;
    }

    /**
     * Return the pointer to the underlying buffer.
     */
    template <class CT, class... S>
    template <class T>
    inline auto xview<CT, S...>::data() const ->
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, const_pointer>
    {
        return m_e.data();
    }

    template <class CT, class... S>
    template <class T>
    inline auto xview<CT, S...>::data() ->
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, pointer>
    {
        return m_e.data();
    }

    template <class CT, class... S>
    template <std::size_t... I>
    inline std::size_t xview<CT, S...>::data_offset_impl(std::index_sequence<I...>) const noexcept
    {
        auto temp = std::array<std::ptrdiff_t, sizeof...(S)>({
            (static_cast<ptrdiff_t>(xt::value(std::get<I>(m_slices), 0)) * m_e.strides()[I - newaxis_count_before<S...>(I)])...
        });

        std::ptrdiff_t result = 0;
        for (std::size_t i = 0; i < sizeof...(S); ++i)
        {
            result += temp[i];
        }
        return static_cast<std::size_t>(result) + m_e.data_offset();
    }

    /**
     * Return the offset to the first element of the view in the underlying container.
     */
    template <class CT, class... S>
    template <class T>
    inline auto xview<CT, S...>::data_offset() const noexcept ->
        std::enable_if_t<has_data_interface<T>::value && is_strided_view, std::size_t>
    {
        if (!m_strides_computed)
        {
            compute_strides(std::integral_constant<bool, is_contiguous_view>{});
        }
        return m_data_offset;
    }
    //@}

    template <class CT, class... S>
    inline auto xview<CT, S...>::underlying_size(size_type dim) const -> size_type
    {
        return m_e.shape()[dim];
    }

    template <class CT, class... S>
    inline auto xview<CT, S...>::operator&() & -> xtl::xclosure_pointer<self_type&>
    {
        return xtl::closure_pointer(*this);
    }

    template <class CT, class... S>
    inline auto xview<CT, S...>::operator&() const & -> xtl::xclosure_pointer<const self_type&>
    {
        return xtl::closure_pointer(*this);
    }

    template <class CT, class... S>
    inline auto xview<CT, S...>::operator&() && -> xtl::xclosure_pointer<self_type>
    {
        return xtl::closure_pointer(std::move(*this));
    }

    /**
     * @name Broadcasting
     */
    //@{
    /**
     * Broadcast the shape of the view to the specified parameter.
     * @param shape the result shape
     * @param reuse_cache parameter for internal optimization
     * @return a boolean indicating whether the broadcasting is trivial
     */
    template <class CT, class... S>
    template <class ST>
    inline bool xview<CT, S...>::broadcast_shape(ST& shape, bool) const
    {
        return xt::broadcast_shape(m_shape, shape);
    }

    /**
     * Compares the specified strides with those of the view to see whether
     * the broadcasting is trivial.
     * @return a boolean indicating whether the broadcasting is trivial
     */
    template <class CT, class... S>
    template <class ST>
    inline bool xview<CT, S...>::is_trivial_broadcast(const ST& str) const
    {
        return xtl::mpl::static_if<is_strided_view>([&](auto self)
        {
            return str.size() == self(this)->strides().size() &&
                std::equal(str.cbegin(), str.cend(), self(this)->strides().begin());

        }, /*else*/ [](auto /*self*/){
            return false;
        });
    }
    //@}

    template <class CT, class... S>
    template <class It>
    inline It xview<CT, S...>::data_xbegin_impl(It begin) const noexcept
    {
        return begin + data_offset();
    }

    template <class CT, class... S>
    template <class It>
    inline It xview<CT, S...>::data_xend_impl(It begin, layout_type l) const noexcept
    {
        std::ptrdiff_t end_offset = static_cast<std::ptrdiff_t>(std::accumulate(backstrides().begin(), backstrides().end(), std::size_t(0)));
        return strided_data_end(*this, begin + end_offset + 1, l);
    }

    template <class CT, class... S>
    inline auto xview<CT, S...>::data_xbegin() noexcept -> container_iterator
    {
        return data_xbegin_impl(data());
    }

    template <class CT, class... S>
    inline auto xview<CT, S...>::data_xbegin() const noexcept -> const_container_iterator
    {
        return data_xbegin_impl(data());
    }

    template <class CT, class... S>
    inline auto xview<CT, S...>::data_xend(layout_type l) noexcept -> container_iterator
    {
        return data_xend_impl(data() + data_offset(), l);
    }

    template <class CT, class... S>
    inline auto xview<CT, S...>::data_xend(layout_type l) const noexcept -> const_container_iterator
    {
        return data_xend_impl(data() + data_offset(), l);
    }

    // Assign to operator enabled for contigous views
    template <class CT, class... S>
    template <class E, class T, class>
    void xview<CT, S...>::assign_to(xexpression<E>& e, bool force_resize) const
    {
        auto& de = e.derived_cast();
        de.resize(shape(), force_resize);
        std::copy(data() + data_offset(), data() + data_offset() + de.size(), de.template begin<static_layout>());
    }

    template <class CT, class... S>
    template <class align, class simd, class T>
    inline auto xview<CT, S...>::store_simd(size_type i, const simd& e) -> enable_simd_interface<T, void>
    {
        return m_e.template store_simd<xsimd::unaligned_mode>(data_offset() + i, e);
    }

    template <class CT, class... S>
    template <class align, class simd, class T>
    inline auto xview<CT, S...>::load_simd(size_type i) const -> enable_simd_interface<T, simd_return_type<simd>>
    {
        return m_e.template load_simd<xsimd::unaligned_mode, simd>(data_offset() + i);
    }

    template <class CT, class... S>
    template <class T>
    inline auto xview<CT, S...>::data_element(size_type i) -> enable_simd_interface<T, reference>
    {
        return m_e.data_element(data_offset() + i);
    }

    template <class CT, class... S>
    template <class T>
    inline auto xview<CT, S...>::data_element(size_type i) const -> enable_simd_interface<T, const_reference>
    {
        return m_e.data_element(data_offset() + i);
    }

    template <class CT, class... S>
    template <class... Args>
    inline auto xview<CT, S...>::make_index_sequence(Args...) const noexcept
    {
        return std::make_index_sequence<(sizeof...(Args)+integral_count<S...>() > newaxis_count<S...>() ?
                                         sizeof...(Args)+integral_count<S...>() - newaxis_count<S...>() :
                                         0)>();
    }

    template <class CT, class... S>
    template <std::size_t... I>
    inline auto xview<CT, S...>::compute_strides_impl(std::index_sequence<I...>) const noexcept
    {
        return std::array<std::ptrdiff_t, sizeof...(I)>({
            (static_cast<std::ptrdiff_t>(xt::step_size(std::get<integral_skip<S...>(I)>(m_slices), 1)) * m_e.strides()[integral_skip<S...>(I) - newaxis_count_before<S...>(integral_skip<S...>(I))])...
        });
    }

    template <class CT, class... S>
    inline void xview<CT, S...>::compute_strides(std::false_type) const
    {
        m_strides = xtl::make_sequence<inner_strides_type>(dimension(), 0);
        m_backstrides = xtl::make_sequence<inner_strides_type>(dimension(), 0);

        constexpr std::size_t n_strides = sizeof...(S) - integral_count<S...>();

        auto slice_strides = compute_strides_impl(std::make_index_sequence<n_strides>());

        for (std::size_t i = 0; i < n_strides; ++i)
        {
            m_strides[i] = slice_strides[i];
            // adapt strides for shape[i] == 1 to make consistent with rest of xtensor
            detail::adapt_strides(shape(), m_strides, &m_backstrides, i);
        }
        for (std::size_t i = n_strides; i < dimension(); ++i)
        {
            m_strides[i] = m_e.strides()[i + integral_count<S...>() - newaxis_count<S...>()];
            detail::adapt_strides(shape(), m_strides, &m_backstrides, i);
        }

        m_data_offset = data_offset_impl(std::make_index_sequence<sizeof...(S)>());
    }

    template <class CT, class... S>
    inline void xview<CT, S...>::compute_strides(std::true_type) const
    {
    }

    template <class CT, class... S>
    inline auto xview<CT, S...>::access() -> reference
    {
        return access_impl(make_index_sequence());
    }

    template <class CT, class... S>
    template <class Arg, class... Args>
    inline auto xview<CT, S...>::access(Arg arg, Args... args) -> reference
    {
        if (sizeof...(Args) >= dimension())
        {
            return access(args...);
        }
        return access_impl(make_index_sequence(arg, args...), arg, args...);
    }

    template <class CT, class... S>
    inline auto xview<CT, S...>::access() const -> const_reference
    {
        return access_impl(make_index_sequence());
    }

    template <class CT, class... S>
    template <class Arg, class... Args>
    inline auto xview<CT, S...>::access(Arg arg, Args... args) const -> const_reference
    {
        if (sizeof...(Args) >= dimension())
        {
            return access(args...);
        }
        return access_impl(make_index_sequence(arg, args...), arg, args...);
    }

    template <class CT, class... S>
    template <typename std::decay_t<CT>::size_type... I, class... Args>
    inline auto xview<CT, S...>::unchecked_impl(std::index_sequence<I...>, Args... args) -> reference
    {
        return m_e.unchecked(index<I>(args...)...);
    }

    template <class CT, class... S>
    template <typename std::decay_t<CT>::size_type... I, class... Args>
    inline auto xview<CT, S...>::unchecked_impl(std::index_sequence<I...>, Args... args) const -> const_reference
    {
        return m_e.unchecked(index<I>(args...)...);
    }

    template <class CT, class... S>
    template <typename std::decay_t<CT>::size_type... I, class... Args>
    inline auto xview<CT, S...>::access_impl(std::index_sequence<I...>, Args... args) -> reference
    {
        return m_e(index<I>(args...)...);
    }

    template <class CT, class... S>
    template <typename std::decay_t<CT>::size_type... I, class... Args>
    inline auto xview<CT, S...>::access_impl(std::index_sequence<I...>, Args... args) const -> const_reference
    {
        return m_e(index<I>(args...)...);
    }

    template <class CT, class... S>
    template <typename std::decay_t<CT>::size_type I, class... Args>
    inline auto xview<CT, S...>::index(Args... args) const -> std::enable_if_t<lesser_condition<I>::value, size_type>
    {
        return sliced_access<I - integral_count_before<S...>(I) + newaxis_count_before<S...>(I + 1)>
            (std::get<I + newaxis_count_before<S...>(I + 1)>(m_slices), args...);
    }

    template <class CT, class... S>
    template <typename std::decay_t<CT>::size_type I, class... Args>
    inline auto xview<CT, S...>::index(Args... args) const -> std::enable_if_t<!lesser_condition<I>::value, size_type>
    {
        return argument<I - integral_count<S...>() + newaxis_count<S...>()>(args...);
    }

    template <class CT, class... S>
    template <typename std::decay_t<CT>::size_type I, class T>
    inline auto xview<CT, S...>::sliced_access(const xslice<T>& slice) const -> size_type
    {
        return static_cast<size_type>(slice.derived_cast()(0));
    }

    template <class CT, class... S>
    template <typename std::decay_t<CT>::size_type I, class T, class Arg, class... Args>
    inline auto xview<CT, S...>::sliced_access(const xslice<T>& slice, Arg arg, Args... args) const -> size_type
    {
        using ST = typename T::size_type;
        return static_cast<size_type>(slice.derived_cast()(argument<I>(static_cast<ST>(arg), static_cast<ST>(args)...)));
    }

    template <class CT, class... S>
    template <typename std::decay_t<CT>::size_type I, class T, class... Args>
    inline auto xview<CT, S...>::sliced_access(const T& squeeze, Args...) const -> disable_xslice<T, size_type>
    {
        return static_cast<size_type>(squeeze);
    }

    template <class CT, class... S>
    template <class It>
    inline auto xview<CT, S...>::make_index(It first, It last) const -> base_index_type
    {
        auto index = xtl::make_sequence<base_index_type>(m_e.dimension(), 0);
        auto func1 = [&first](const auto& s) {
            return get_slice_value(s, first);
        };
        auto func2 = [](const auto& s) {
            return xt::value(s, 0);
        };
        for (size_type i = 0; i != m_e.dimension(); ++i)
        {
            size_type k = newaxis_skip<S...>(i);
            std::advance(first, k - i);
            if (first != last)
            {
                index[i] = k < sizeof...(S) ?
                    apply<size_type>(k, func1, m_slices) : *first++;
            }
            else
            {
                index[i] = k < sizeof...(S) ?
                    apply<size_type>(k, func2, m_slices) : 0;
            }
        }
        return index;
    }

    namespace xview_detail
    {
        template <class V, class T>
        inline void run_assign_temporary_impl(V& v, const T& t, std::true_type /* enable strided assign */)
        {
            strided_assign(v, t, std::true_type{});
        }

        template <class V, class T>
        inline void run_assign_temporary_impl(V& v, const T& t, std::false_type /* fallback to iterator assign */)
        {
            std::copy(t.cbegin(), t.cend(), v.begin());
        }
    }

    template <class CT, class... S>
    inline void xview<CT, S...>::assign_temporary_impl(temporary_type&& tmp)
    {
        constexpr bool fast_assign = detail::is_strided_view<xexpression_type, S...>::value && \
                                     xassign_traits<xview<CT, S...>, temporary_type>::simd_strided_loop();
        xview_detail::run_assign_temporary_impl(*this, tmp, std::integral_constant<bool, fast_assign>{});
    }

    namespace detail
    {
        template <class E, class... S>
        inline std::size_t get_underlying_shape_index(std::size_t I)
        {
            return I - newaxis_count_before<get_slice_type<E, S>...>(I);
        }

        template <class... S>
        struct check_slice;

        template <>
        struct check_slice<>
        {
            using type = void_t<>;
        };

        template <class S, class... SL>
        struct check_slice<S, SL...>
        {
            static_assert(!std::is_same<S, xellipsis_tag>::value, "ellipsis not supported vith xview");
            using type = typename check_slice<SL...>::type;
        };

        template <class E, std::size_t... I, class... S>
        inline auto make_view_impl(E&& e, std::index_sequence<I...>, S&&... slices)
        {
            // Checks that no ellipsis slice is used
            using view_type = xview<xtl::closure_type_t<E>, get_slice_type<std::decay_t<E>, S>...>;
            return view_type(std::forward<E>(e),
                get_slice_implementation(e, std::forward<S>(slices), get_underlying_shape_index<std::decay_t<E>, S...>(I))...
            );
        }
    }

    /**
     * Constructs and returns a view on the specified xexpression. Users
     * should not directly construct the slices but call helper functions
     * instead.
     * @param e the xexpression to adapt
     * @param slices the slices list describing the view
     * @sa range, all, newaxis
     */
    template <class E, class... S>
    inline auto view(E&& e, S&&... slices)
    {
        return detail::make_view_impl(std::forward<E>(e), std::make_index_sequence<sizeof...(S)>(), std::forward<S>(slices)...);
    }

    /***************
     * stepper api *
     ***************/

    template <class CT, class... S>
    template <class ST, bool Enable>
    inline auto xview<CT, S...>::stepper_begin(const ST& shape) -> std::enable_if_t<!Enable, stepper>
    {
        size_type offset = shape.size() - dimension();
        return stepper(this, m_e.stepper_begin(m_e.shape()), offset);
    }

    template <class CT, class... S>
    template <class ST, bool Enable>
    inline auto xview<CT, S...>::stepper_end(const ST& shape, layout_type l) -> std::enable_if_t<!Enable, stepper>
    {
        size_type offset = shape.size() - dimension();
        return stepper(this, m_e.stepper_end(m_e.shape(), l), offset, true, l);
    }

    template <class CT, class... S>
    template <class ST, bool Enable>
    inline auto xview<CT, S...>::stepper_begin(const ST& shape) const -> std::enable_if_t<!Enable, const_stepper>
    {
        size_type offset = shape.size() - dimension();
        const xexpression_type& e = m_e;
        return const_stepper(this, e.stepper_begin(m_e.shape()), offset);
    }

    template <class CT, class... S>
    template <class ST, bool Enable>
    inline auto xview<CT, S...>::stepper_end(const ST& shape, layout_type l) const -> std::enable_if_t<!Enable, const_stepper>
    {
        size_type offset = shape.size() - dimension();
        const xexpression_type& e = m_e;
        return const_stepper(this, e.stepper_end(m_e.shape(), l), offset, true, l);
    }

    template <class CT, class... S>
    template <class ST, bool Enable>
    inline auto xview<CT, S...>::stepper_begin(const ST& shape) -> std::enable_if_t<Enable, stepper>
    {
        size_type offset = shape.size() - dimension();
        return stepper(this, data_xbegin(), offset);
    }

    template <class CT, class... S>
    template <class ST, bool Enable>
    inline auto xview<CT, S...>::stepper_end(const ST& shape, layout_type l) -> std::enable_if_t<Enable, stepper>
    {
        size_type offset = shape.size() - dimension();
        return stepper(this, data_xend(l), offset);
    }

    template <class CT, class... S>
    template <class ST, bool Enable>
    inline auto xview<CT, S...>::stepper_begin(const ST& shape) const -> std::enable_if_t<Enable, const_stepper>
    {
        size_type offset = shape.size() - dimension();
        return const_stepper(this, data_xbegin(), offset);
    }

    template <class CT, class... S>
    template <class ST, bool Enable>
    inline auto xview<CT, S...>::stepper_end(const ST& shape, layout_type l) const-> std::enable_if_t<Enable, const_stepper>
    {
        size_type offset = shape.size() - dimension();
        return const_stepper(this, data_xend(l), offset);
    }

    /********************************
     * xview_stepper implementation *
     ********************************/

    template <bool is_const, class CT, class... S>
    inline xview_stepper<is_const, CT, S...>::xview_stepper(view_type* view, substepper_type it,
                                                            size_type offset, bool end, layout_type l)
        : p_view(view), m_it(it), m_offset(offset)
    {
        if (!end)
        {
            std::fill(m_index_keeper.begin(), m_index_keeper.end(), 0);
            auto func = [](const auto& s) { return xt::value(s, 0); };
            for (size_type i = 0; i < sizeof...(S); ++i)
            {
                if (!is_newaxis_slice(i))
                {
                    size_type s = apply<size_type>(i, func, p_view->slices());
                    size_type index = i - newaxis_count_before<S...>(i);
                    m_it.step(index, s);
                }
            }
        }
        else
        {
            to_end_impl(l);
        }
    }

    template <bool is_const, class CT, class... S>
    inline auto xview_stepper<is_const, CT, S...>::operator*() const -> reference
    {
        return *m_it;
    }

    template <bool is_const, class CT, class... S>
    inline void xview_stepper<is_const, CT, S...>::step(size_type dim)
    {
        auto func = [this](size_type index, size_type offset) { m_it.step(index, offset); };
        common_step_forward(dim, func);
    }

    template <bool is_const, class CT, class... S>
    inline void xview_stepper<is_const, CT, S...>::step_back(size_type dim)
    {
        auto func = [this](size_type index, size_type offset) {
            m_it.step_back(index, offset);
        };
        common_step_backward(dim, func);
    }

    template <bool is_const, class CT, class... S>
    inline void xview_stepper<is_const, CT, S...>::step(size_type dim, size_type n)
    {
        auto func = [this](size_type index, size_type offset) { m_it.step(index, offset); };
        common_step_forward(dim, n, func);
    }

    template <bool is_const, class CT, class... S>
    inline void xview_stepper<is_const, CT, S...>::step_back(size_type dim, size_type n)
    {
        auto func = [this](size_type index, size_type offset) { 
            m_it.step_back(index, offset);
        };
        common_step_backward(dim, n, func);
    }

    template <bool is_const, class CT, class... S>
    inline void xview_stepper<is_const, CT, S...>::reset(size_type dim)
    {
        auto func = [this](size_type index, size_type offset) { m_it.step_back(index, offset); };
        common_reset(dim, func, false);
    }

    template <bool is_const, class CT, class... S>
    inline void xview_stepper<is_const, CT, S...>::reset_back(size_type dim)
    {
        auto func = [this](size_type index, size_type offset) { m_it.step(index, offset); };
        common_reset(dim, func, true);
    }

    template <bool is_const, class CT, class... S>
    inline void xview_stepper<is_const, CT, S...>::to_begin()
    {
        std::fill(m_index_keeper.begin(), m_index_keeper.end(), 0);
        m_it.to_begin();
    }

    template <bool is_const, class CT, class... S>
    inline void xview_stepper<is_const, CT, S...>::to_end(layout_type l)
    {
        m_it.to_end(l);
        to_end_impl(l);
    }

    template <bool is_const, class CT, class... S>
    inline bool xview_stepper<is_const, CT, S...>::is_newaxis_slice(size_type index) const noexcept
    {
        // A bit tricky but avoids a lot of template instantiations
        return newaxis_count_before<S...>(index + 1) != newaxis_count_before<S...>(index);
    }

    template <bool is_const, class CT, class... S>
    inline void xview_stepper<is_const, CT, S...>::to_end_impl(layout_type l)
    {
        auto func = [](const auto& s) {
            return xt::value(s, get_size(s) - 1);
        };
        auto size_func = [](const auto& s) {
            return get_size(s);
        };

        for (size_type i = 0; i < sizeof...(S); ++i)
        {
            if (!is_newaxis_slice(i))
            {
                size_type s = apply<size_type>(i, func, p_view->slices());
                size_type ix = apply<size_type>(i, size_func, p_view->slices());
                m_index_keeper[i] = ix;
                size_type index = i - newaxis_count_before<S...>(i);
                s = p_view->underlying_size(index) - 1 - s;
                m_it.step_back(index, s);
            }
        }
        if (l == layout_type::row_major)
        {
            for (size_type i = sizeof...(S); i > 0; --i)
            {
                if (!is_newaxis_slice(i - 1))
                {
                    m_index_keeper[i - 1]++;
                    break;
                }
            }
        }
        else if (l == layout_type::column_major)
        {
            for (size_type i = 0; i < sizeof...(S); ++i)
            {
                if (!is_newaxis_slice(i))
                {
                    m_index_keeper[i]++;
                    break;
                }
            }
        }
        else
        {
            throw std::runtime_error("Iteration only allowed in row or column major.");
        }
    }

    template <bool is_const, class CT, class... S>
    template <class F>
    void xview_stepper<is_const, CT, S...>::common_step_forward(size_type dim, F f)
    {
        if (dim >= m_offset)
        {
            auto func = [&dim, this](const auto& s) noexcept {
                this->m_index_keeper[dim]++;
                return step_size(s, this->m_index_keeper[dim], 1);
            };
            size_type index = integral_skip<S...>(dim);
            if (!is_newaxis_slice(index))
            {
                size_type step_size = index < sizeof...(S) ?
                    apply<size_type>(index, func, p_view->slices()) : 1;
                index -= newaxis_count_before<S...>(index);
                f(index, step_size);
            }
        }
    }

    template <bool is_const, class CT, class... S>
    template <class F>
    void xview_stepper<is_const, CT, S...>::common_step_forward(size_type dim, size_type n, F f)
    {
        if (dim >= m_offset)
        {
            auto func = [&dim, &n, this](const auto& s) noexcept {
                this->m_index_keeper[dim] += n;
                return step_size(s, this->m_index_keeper[dim], n);
            };

            size_type index = integral_skip<S...>(dim);
            if (!is_newaxis_slice(index))
            {
                size_type step_size = index < sizeof...(S) ?
                    apply<size_type>(index, func, p_view->slices()) : n;
                index -= newaxis_count_before<S...>(index);
                f(index, step_size);
            }
        }
    }

    template <bool is_const, class CT, class... S>
    template <class F>
    void xview_stepper<is_const, CT, S...>::common_step_backward(size_type dim, F f)
    {
        if (dim >= m_offset)
        {
            auto func = [&dim, this](const auto& s) noexcept {
                this->m_index_keeper[dim]--;
                return step_size(s, this->m_index_keeper[dim], 1);
            };
            size_type index = integral_skip<S...>(dim);
            if (!is_newaxis_slice(index))
            {
                size_type step_size = index < sizeof...(S) ?
                    apply<size_type>(index, func, p_view->slices()) : 1;
                index -= newaxis_count_before<S...>(index);
                f(index, step_size);
            }
        }
    }

    template <bool is_const, class CT, class... S>
    template <class F>
    void xview_stepper<is_const, CT, S...>::common_step_backward(size_type dim, size_type n, F f)
    {
        if (dim >= m_offset)
        {
            auto func = [&dim, &n, this](const auto& s) noexcept {
                this->m_index_keeper[dim] -= n;
                return step_size(s, this->m_index_keeper[dim], n);
            };

            size_type index = integral_skip<S...>(dim);
            if (!is_newaxis_slice(index))
            {
                size_type step_size = index < sizeof...(S) ?
                    apply<size_type>(index, func, p_view->slices()) : n;
                index -= newaxis_count_before<S...>(index);
                f(index, step_size);
            }
        }
    }

    template <bool is_const, class CT, class... S>
    template <class F>
    void xview_stepper<is_const, CT, S...>::common_reset(size_type dim, F f, bool backwards)
    {
        auto size_func = [](const auto& s) noexcept { return get_size(s); };
        auto end_func = [](const auto& s) noexcept { return xt::value(s, get_size(s) - 1) - xt::value(s, 0); };

        size_type index = integral_skip<S...>(dim);
        if (!is_newaxis_slice(index))
        {
            size_type size = index < sizeof...(S) ? apply<size_type>(index, size_func, p_view->slices()) : p_view->shape()[dim];
            if (size != 0)
            {
                size = size - 1;
            }

            size_type sz = index < sizeof...(S) ? apply<size_type>(index, size_func, p_view->slices()) : p_view->shape()[dim];
            if (dim < m_index_keeper.size())
            {
                m_index_keeper[dim] = backwards ? sz : 0;
            }

            auto ss = index < sizeof...(S) ? apply<size_type>(index, end_func, p_view->slices()) : p_view->shape()[dim];
            size_type reset_n = index < sizeof...(S) ? ss : size;
            index -= newaxis_count_before<S...>(index);
            f(index, reset_n);
        }
    }
}

#endif
