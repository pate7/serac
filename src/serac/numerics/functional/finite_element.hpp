// Copyright (c) 2019-2022, Lawrence Livermore National Security, LLC and
// other Serac Project Developers. See the top-level LICENSE file for
// details.
//
// SPDX-License-Identifier: (BSD-3-Clause)

/**
 * @file finite_element.hpp
 *
 * @brief This file contains helper traits and enumerations for classifying
 * finite elements
 */
#pragma once

#include "tuple.hpp"
#include "tensor.hpp"
#include "polynomials.hpp"

namespace serac {

template <int q>
struct TensorProductQuadratureRule {
  tensor<double, q> weights1D;
  tensor<double, q> points1D;

  SERAC_HOST_DEVICE double weight(int ix, int iy) const { return weights1D[ix] * weights1D[iy]; }
  SERAC_HOST_DEVICE double weight(int ix, int iy, int iz) const { return weights1D[ix] * weights1D[iy] * weights1D[iz]; }
};

/**
 * @brief Element geometries
 */
enum class Geometry
{
  Point,
  Segment,
  Triangle,
  Quadrilateral,
  Tetrahedron,
  Hexahedron
};

/**
 * @brief Compile-time alias for a dimension
 */
template <int d>
struct Dimension {
  /**
   * @brief Returns the dimension
   */
  constexpr operator int() { return d; }
};

template <Geometry g, int q>
SERAC_HOST_DEVICE constexpr int num_quadrature_points() {
  if (g == Geometry::Segment) { return q; }
  if (g == Geometry::Quadrilateral) { return q * q; }
  if (g == Geometry::Hexahedron) { return q * q * q; }
  return -1;
}

template <Geometry g, int q>
struct batched_jacobian;

template <int q>
struct batched_jacobian<Geometry::Hexahedron, q> {
  using type = tensor<double, 3, 3, q * q * q>;
};

template <int q>
struct batched_jacobian<Geometry::Quadrilateral, q> {
  using type = tensor<double, 2, 2, q * q>;
};

template <Geometry g, int q>
struct batched_position;

template <int q>
struct batched_position<Geometry::Hexahedron, q> {
  using type = tensor<double, 3, q * q * q>;
};

template <int q>
struct batched_position<Geometry::Quadrilateral, q> {
  using type = tensor<double, 2, q * q>;
};

template <int q>
struct batched_position<Geometry::Segment, q> {
  using type = tensor<double, q>;
};

template <Geometry g>
SERAC_HOST_DEVICE constexpr int elements_per_block(int q)
{
  if (g == Geometry::Hexahedron) {
    switch (q) {
      case 1:
        return 64;
      case 2:
        return 16;
      case 3:
        return 4;
      default:
        return 1;
    }
  }

  if (g == Geometry::Quadrilateral) {
    switch (q) {
      case 1:
        return 128;
      case 2:
        return 32;
      case 3:
        return 16;
      case 4:
        return 8;
      default:
        return 1;
    }
  }
}

/**
 * @brief Returns the dimension of an element geometry
 * @param[in] g The @p Geometry to retrieve the dimension of
 */
SERAC_HOST_DEVICE constexpr int dimension_of(Geometry g)
{
  if (g == Geometry::Segment) {
    return 1;
  }

  if (g == Geometry::Triangle || g == Geometry::Quadrilateral) {
    return 2;
  }

  if (g == Geometry::Tetrahedron || g == Geometry::Hexahedron) {
    return 3;
  }

  return -1;
}

/**
 * @brief Element conformity
 *
 * QOI   denotes a "quantity of interest", implying integration with the test function "1"
 * H1    denotes a function space where values are continuous across element boundaries
 * HCURL denotes a vector-valued function space where only the tangential component is continuous across element
 * boundaries HDIV  denotes a vector-valued function space where only the normal component is continuous across element
 * boundaries L2    denotes a function space where values are discontinuous across element boundaries
 */
enum class Family
{
  QOI,
  H1,
  HCURL,
  HDIV,
  L2
};

/**
 * @brief H1 elements of order @p p
 * @tparam p The order of the elements
 * @tparam c The vector dimension
 */
template <int p, int c = 1>
struct H1 {
  static constexpr int    order      = p;           ///< the polynomial order of the elements
  static constexpr int    components = c;           ///< the number of components at each node
  static constexpr Family family     = Family::H1;  ///< the family of the basis functions
};

/**
 * @brief H(curl) elements of order @p p
 * @tparam p The order of the elements
 * @tparam c The vector dimension
 */
template <int p, int c = 1>
struct Hcurl {
  static constexpr int    order      = p;              ///< the polynomial order of the elements
  static constexpr int    components = c;              ///< the number of components at each node
  static constexpr Family family     = Family::HCURL;  ///< the family of the basis functions
};

/**
 * @brief Discontinuous elements of order @p p
 * @tparam p The order of the elements
 * @tparam c The vector dimension
 */
template <int p, int c = 1>
struct L2 {
  static constexpr int    order      = p;           ///< the polynomial order of the elements
  static constexpr int    components = c;           ///< the number of components at each node
  static constexpr Family family     = Family::L2;  ///< the family of the basis functions
};

/**
 * @brief "Quantity of Interest" elements (i.e. elements with a single shape function, 1)
 */
struct QOI {
  static constexpr int    order      = 0;            ///< the polynomial order of the elements
  static constexpr int    components = 1;            ///< the number of components at each node
  static constexpr Family family     = Family::QOI;  ///< the family of the basis functions
};

template <Family f, typename T, int q, int dim>
void parent_to_physical(tensor< T, q > & qf_input, const tensor<double, dim, dim, q >& jacobians)
{
  [[maybe_unused]] constexpr int VALUE = 0;
  [[maybe_unused]] constexpr int DERIVATIVE = 1;

  for (int k = 0; k < q; k++) {
    tensor<double, dim, dim> J;
    for (int row = 0; row < dim; row++) {
      for (int col = 0; col < dim; col++) {
        J[row][col] = jacobians(col, row, k);
      }
    }

    if constexpr (f == Family::H1) {
      // note: no transformation necessary for the values of H1-field
      get<DERIVATIVE>(qf_input[k]) = dot(get<DERIVATIVE>(qf_input[k]), inv(J));
    }

    if constexpr (f == Family::HCURL) {
      get<VALUE>(qf_input[k]) = dot(get<VALUE>(qf_input[k]), inv(J));
      get<DERIVATIVE>(qf_input[k]) = get<DERIVATIVE>(qf_input[k]) / det(J);
      if constexpr (dim == 3) {
        get<DERIVATIVE>(qf_input[k]) = dot(get<DERIVATIVE>(qf_input[k]), transpose(J));
      }
    }

  }

}

template <Family f, typename T, int q, int dim>
void physical_to_parent(tensor< T, q > & qf_output, const tensor<double, dim, dim, q >& jacobians)
{
  constexpr int SOURCE = 0;
  constexpr int FLUX = 1;

  for (int k = 0; k < q; k++) {
    tensor<double, dim, dim> J_T;
    for (int row = 0; row < dim; row++) {
      for (int col = 0; col < dim; col++) {
        J_T[row][col] = jacobians(row, col, k);
      }
    }

    auto dv = det(J_T);

    if constexpr (f == Family::H1) {
      get<SOURCE>(qf_output[k]) = get<SOURCE>(qf_output[k]) * dv;
      get<FLUX>(qf_output[k]) = dot(get<FLUX>(qf_output[k]), inv(J_T)) * dv;
    }

    // note: the flux term here is usually divided by detJ, but 
    // physical_to_parent also multiplies every quadrature-point value by det(J)
    // so that part cancels out
    if constexpr (f == Family::HCURL) {
      get<SOURCE>(qf_output[k]) = dot(get<SOURCE>(qf_output[k]), inv(J_T)) * dv;
      if constexpr (dim == 3) {
        get<FLUX>(qf_output[k]) = dot(get<FLUX>(qf_output[k]), transpose(J_T));
      }
    }

    if constexpr (f == Family::QOI) {
      qf_output[k] = qf_output[k] * dv;
    }

  }

}

/**
 * @brief Template prototype for finite element implementations
 * @tparam g The geometry of the element
 * @tparam family The continuity of the element
 * the implementations of the different finite element specializations
 * are in .inl files in the detail/ directory.
 *
 * In each of these files, the finite_element specialization
 * should implement the following concept:
 *
 * struct finite_element< some_geometry, some_space > > {
 *   static constexpr Geometry geometry = ...; ///< one of Triangle, Quadrilateral, etc
 *   static constexpr Family family     = ...; ///< one of H1, HCURL, HDIV, etc
 *   static constexpr int  components   = ...; ///< how many components per node
 *   static constexpr int  dim          = ...; ///< number of parent element coordinates
 *   static constexpr int  ndof         = ...; ///< how many degrees of freedom for an element with 1 component per node
 *
 *   /// implement the way this element type interpolates the solution on the interior of the element
 *   static constexpr auto shape_functions(tensor<double, dim> xi) { ... }
 *
 *   /// implement the derivatives of this element's shape functions w.r.t. parent element coordinates
 *   static constexpr auto shape_function_derivatives(tensor<double, dim> xi) { ... }
 * };
 *
 */
template <Geometry g, typename family>
struct finite_element;

#include "detail/segment_H1.inl"
#include "detail/segment_Hcurl.inl"
#include "detail/segment_L2.inl"

#include "detail/quadrilateral_H1.inl"
#include "detail/quadrilateral_Hcurl.inl"
#include "detail/quadrilateral_L2.inl"

#include "detail/hexahedron_H1.inl"
#include "detail/hexahedron_Hcurl.inl"
#include "detail/hexahedron_L2.inl"

#include "detail/qoi.inl"

}  // namespace serac
