// Copyright (c) 2019-2022, Lawrence Livermore National Security, LLC and
// other Serac Project Developers. See the top-level LICENSE file for
// details.
//
// SPDX-License-Identifier: (BSD-3-Clause)

#include "serac/physics/solid_mechanics.hpp"

#include <functional>
#include <fstream>
#include <set>
#include <string>

#include "axom/slic/core/SimpleLogger.hpp"
#include <gtest/gtest.h>
#include "mfem.hpp"

#include "serac/mesh/mesh_utils.hpp"
#include "serac/physics/state/state_manager.hpp"
#include "serac/physics/materials/solid_material.hpp"
#include "serac/physics/materials/parameterized_solid_material.hpp"
#include "serac/serac_config.hpp"

namespace serac {

using solid_mechanics::default_dynamic_options;
using solid_mechanics::default_static_options;
using solid_mechanics::direct_static_options;

template <int p, int dim>
void functional_solid_test_static(double expected_disp_norm)
{
  MPI_Barrier(MPI_COMM_WORLD);

  int serial_refinement   = 0;
  int parallel_refinement = 0;

  // Create DataStore
  axom::sidre::DataStore datastore;
  serac::StateManager::initialize(datastore, "solid_functional_static_solve");

  static_assert(dim == 2 || dim == 3, "Dimension must be 2 or 3 for solid functional test");

  // Construct the appropriate dimension mesh and give it to the data store
  std::string filename =
      (dim == 2) ? SERAC_REPO_DIR "/data/meshes/beam-quad.mesh" : SERAC_REPO_DIR "/data/meshes/beam-hex.mesh";

  auto mesh = mesh::refineAndDistribute(buildMeshFromFile(filename), serial_refinement, parallel_refinement);
  serac::StateManager::setMesh(std::move(mesh));

  // Define a boundary attribute set
  std::set<int> ess_bdr = {1};

  // Use a direct solver (DSuperLU) for the Jacobian solve
  SolverOptions options = {DirectSolverOptions{}, solid_mechanics::default_nonlinear_options};

  // Construct a functional-based solid mechanics solver
  SolidMechanics<p, dim> solid_solver(options, GeometricNonlinearities::On, "solid_functional");

  solid_mechanics::NeoHookean mat{1.0, 1.0, 1.0};
  solid_solver.setMaterial(mat);

  // Define the function for the initial displacement and boundary condition
  auto bc = [](const mfem::Vector&, mfem::Vector& bc_vec) -> void { bc_vec = 0.0; };

  // Set the initial displacement and boundary condition
  solid_solver.setDisplacementBCs(ess_bdr, bc);
  solid_solver.setDisplacement(bc);

  tensor<double, dim> constant_force;

  constant_force[0] = 0.0;
  constant_force[1] = 5.0e-4;

  if (dim == 3) {
    constant_force[2] = 0.0;
  }

  solid_mechanics::ConstantBodyForce<dim> force{constant_force};
  solid_solver.addBodyForce(force);

  // Finalize the data structures
  solid_solver.completeSetup();

  // Perform the quasi-static solve
  double dt = 1.0;
  solid_solver.advanceTimestep(dt);

  // Output the sidre-based and paraview plot files
  solid_solver.outputState("paraview_output");

  // Check the final displacement norm
  EXPECT_NEAR(expected_disp_norm, norm(solid_solver.displacement()), 1.0e-6);
}

void functional_solid_test_static_J2()
{
  MPI_Barrier(MPI_COMM_WORLD);

  constexpr int p                   = 2;
  constexpr int dim                 = 3;
  int           serial_refinement   = 0;
  int           parallel_refinement = 0;

  // Create DataStore
  axom::sidre::DataStore datastore;
  serac::StateManager::initialize(datastore, "solid_mechanics_J2_test");

  // Construct the appropriate dimension mesh and give it to the data store
  std::string filename = SERAC_REPO_DIR "/data/meshes/beam-hex.mesh";

  auto mesh = mesh::refineAndDistribute(buildMeshFromFile(filename), serial_refinement, parallel_refinement);
  serac::StateManager::setMesh(std::move(mesh));

  auto options           = default_static_options;
  auto linear_options    = solid_mechanics::default_linear_options;
  linear_options.abs_tol = 1.0e-16;  // prevent early-exit in linear solve
  options.linear         = linear_options;

  // Construct a functional-based solid mechanics solver
  SolidMechanics<p, dim> solid_solver(options, GeometricNonlinearities::Off, "solid_mechanics");

  solid_mechanics::J2 mat{
      10000,  // Young's modulus
      0.25,   // Poisson's ratio
      50.0,   // isotropic hardening constant
      5.0,    // kinematic hardening constant
      50.0,   // yield stress
      1.0     // mass density
  };

  solid_mechanics::J2::State initial_state{};

  auto state = solid_solver.createQuadratureDataBuffer(initial_state);

  solid_solver.setMaterial(mat, state);

  // prescribe zero displacement at the supported end of the beam,
  std::set<int> support           = {1};
  auto          zero_displacement = [](const mfem::Vector&, mfem::Vector& u) -> void { u = 0.0; };
  solid_solver.setDisplacementBCs(support, zero_displacement);

  // apply a displacement along z to the the tip of the beam
  auto translated_in_z = [](const mfem::Vector&, double t, mfem::Vector& u) -> void {
    u    = 0.0;
    u[2] = t * (t - 1);
  };
  std::set<int> tip = {2};
  solid_solver.setDisplacementBCs(tip, translated_in_z);

  solid_solver.setDisplacement(zero_displacement);

  // Finalize the data structures
  solid_solver.completeSetup();

  solid_solver.outputState("paraview");

  // Perform the quasi-static solve
  int    num_steps = 10;
  double tmax      = 1.0;
  double dt        = tmax / num_steps;
  for (int i = 0; i < num_steps; i++) {
    solid_solver.advanceTimestep(dt);
    solid_solver.outputState("paraview");
  }

  // this a qualitative test that just verifies
  // that plasticity models can have permanent
  // deformation after unloading
  // EXPECT_LT(norm(solid_solver.reactions()), 1.0e-5);
}

enum class TestType
{
  Pressure,
  Traction
};

template <int p, int dim>
void functional_solid_test_boundary(double expected_disp_norm, TestType test_mode)
{
  MPI_Barrier(MPI_COMM_WORLD);

  int serial_refinement   = 1;
  int parallel_refinement = 0;

  // Create DataStore
  axom::sidre::DataStore datastore;
  serac::StateManager::initialize(datastore, "solid_functional_static_solve");

  static_assert(dim == 2 || dim == 3, "Dimension must be 2 or 3 for solid functional test");

  // Construct the appropriate dimension mesh and give it to the data store
  std::string filename =
      (dim == 2) ? SERAC_REPO_DIR "/data/meshes/beam-quad.mesh" : SERAC_REPO_DIR "/data/meshes/beam-hex.mesh";

  auto mesh = mesh::refineAndDistribute(buildMeshFromFile(filename), serial_refinement, parallel_refinement);
  serac::StateManager::setMesh(std::move(mesh));

  // Construct a functional-based solid mechanics solver
  SolidMechanics<p, dim> solid_solver(default_static_options, GeometricNonlinearities::Off, "solid_functional");

  solid_mechanics::LinearIsotropic mat{1.0, 1.0, 1.0};
  solid_solver.setMaterial(mat);

  // Define the function for the initial displacement and boundary condition
  auto bc = [](const mfem::Vector&, mfem::Vector& bc_vec) -> void { bc_vec = 0.0; };

  // Define a boundary attribute set and specify initial / boundary conditions
  std::set<int> ess_bdr = {1};
  solid_solver.setDisplacementBCs(ess_bdr, bc);
  solid_solver.setDisplacement(bc);

  if (test_mode == TestType::Pressure) {
    solid_solver.setPiolaTraction([](const auto& x, const tensor<double, dim>& n, const double) {
      if (x[0] > 7.5) {
        return 1.0e-2 * n;
      }
      return 0.0 * n;
    });
  } else if (test_mode == TestType::Traction) {
    solid_solver.setPiolaTraction([](const auto& x, const tensor<double, dim>& /*n*/, const double) {
      tensor<double, dim> traction;
      for (int i = 0; i < dim; ++i) {
        traction[i] = (x[0] > 7.9) ? 1.0e-4 : 0.0;
      }
      return traction;
    });
  } else {
    // Default to fail if non-implemented TestType is not implemented
    EXPECT_TRUE(false);
  }

  // Finalize the data structures
  solid_solver.completeSetup();

  // Perform the quasi-static solve
  double dt = 1.0;
  solid_solver.advanceTimestep(dt);

  // Output the sidre-based plot files
  solid_solver.outputState();

  // Check the final displacement norm
  EXPECT_NEAR(expected_disp_norm, norm(solid_solver.displacement()), 1.0e-6);
}

template <typename lambda>
struct ParameterizedBodyForce {
  template <int dim, typename T1, typename T2>
  auto operator()(const tensor<T1, dim> x, double /*t*/, T2 density) const
  {
    return get<0>(density) * acceleration(x);
  }
  lambda acceleration;
};

template <typename T>
ParameterizedBodyForce(T) -> ParameterizedBodyForce<T>;

template <int p, int dim>
void functional_parameterized_solid_test(double expected_disp_norm)
{
  MPI_Barrier(MPI_COMM_WORLD);

  int serial_refinement   = 0;
  int parallel_refinement = 0;

  // Create DataStore
  axom::sidre::DataStore datastore;
  serac::StateManager::initialize(datastore, "solid_functional_parameterized_solve");

  static_assert(dim == 2 || dim == 3, "Dimension must be 2 or 3 for solid functional parameterized test");

  // Construct the appropriate dimension mesh and give it to the data store
  std::string filename =
      (dim == 2) ? SERAC_REPO_DIR "/data/meshes/beam-quad.mesh" : SERAC_REPO_DIR "/data/meshes/beam-hex.mesh";

  auto mesh = mesh::refineAndDistribute(buildMeshFromFile(filename), serial_refinement, parallel_refinement);
  serac::StateManager::setMesh(std::move(mesh));

  // Construct and initialized the user-defined moduli to be used as a differentiable parameter in
  // the solid mechanics physics module.
  FiniteElementState user_defined_shear_modulus(
      StateManager::newState(FiniteElementState::Options{.order = 1, .name = "parameterized_shear"}));

  user_defined_shear_modulus = 1.0;

  FiniteElementState user_defined_bulk_modulus(
      StateManager::newState(FiniteElementState::Options{.order = 1, .name = "parameterized_bulk"}));

  user_defined_bulk_modulus = 1.0;

  // Construct a functional-based solid mechanics solver
  SolidMechanics<p, dim, Parameters<H1<1>, H1<1>>> solid_solver(default_static_options, GeometricNonlinearities::On,
                                                                "solid_functional");
  solid_solver.setParameter(0, user_defined_bulk_modulus);
  solid_solver.setParameter(1, user_defined_shear_modulus);

  solid_mechanics::ParameterizedNeoHookeanSolid<dim> mat{1.0, 0.0, 0.0};
  solid_solver.setMaterial(DependsOn<0, 1>{}, mat);

  // Define the function for the initial displacement and boundary condition
  auto bc = [](const mfem::Vector&, mfem::Vector& bc_vec) -> void { bc_vec = 0.0; };

  // Define a boundary attribute set and specify initial / boundary conditions
  std::set<int> ess_bdr = {1};
  solid_solver.setDisplacementBCs(ess_bdr, bc);
  solid_solver.setDisplacement(bc);

  tensor<double, dim> constant_force;

  constant_force[0] = 0.0;
  constant_force[1] = 5.0e-4;

  if (dim == 3) {
    constant_force[2] = 0.0;
  }

  solid_mechanics::ConstantBodyForce<dim> force{constant_force};
  solid_solver.addBodyForce(force);

  // add some nonexistent body forces / tractions to check that
  // these parameterized versions compile and run without error
  solid_solver.addBodyForce(DependsOn<0>{}, [](const auto& x, double /*t*/, auto /* bulk */) { return x * 0.0; });
  solid_solver.addBodyForce(DependsOn<1>{}, ParameterizedBodyForce{[](const auto& x) { return 0.0 * x; }});
  solid_solver.setPiolaTraction(DependsOn<1>{}, [](const auto& x, auto...) { return 0 * x; });

  // Finalize the data structures
  solid_solver.completeSetup();

  // Perform the quasi-static solve
  double dt = 1.0;
  solid_solver.advanceTimestep(dt);

  // the calculations peformed in these lines of code
  // are not used, but running them as part of this test
  // checks the index-translation part of the derivative
  // kernels is working
  solid_solver.computeSensitivity(0);
  solid_solver.computeSensitivity(1);

  // Output the sidre-based plot files
  solid_solver.outputState();

  // Check the final displacement norm
  EXPECT_NEAR(expected_disp_norm, norm(solid_solver.displacement()), 1.0e-6);
}

TEST(SolidMechanics, 2DQuadParameterizedStatic) { functional_parameterized_solid_test<2, 2>(2.1906312704664623); }

TEST(SolidMechanics, 3DQuadStaticJ2) { functional_solid_test_static_J2(); }

TEST(SolidMechanics, 2DLinearPressure)
{
  functional_solid_test_boundary<1, 2>(0.057051396685822188, TestType::Pressure);
}

}  // namespace serac

int main(int argc, char* argv[])
{
  ::testing::InitGoogleTest(&argc, argv);
  MPI_Init(&argc, &argv);

  axom::slic::SimpleLogger logger;

  int result = RUN_ALL_TESTS();
  MPI_Finalize();

  return result;
}
