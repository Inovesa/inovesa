/******************************************************************************
 * Inovesa - Inovesa Numerical Optimized Vlesov-Equation Solver Application   *
 * Copyright (c) 2014-2018: Patrik Schönfeldt                                 *
 * Copyright (c) 2014-2018: Karlsruhe Institute of Technology                 *
 *                                                                            *
 * This file is part of Inovesa.                                              *
 * Inovesa is free software: you can redistribute it and/or modify            *
 * it under the terms of the GNU General Public License as published by       *
 * the Free Software Foundation, either version 3 of the License, or          *
 * (at your option) any later version.                                        *
 *                                                                            *
 * Inovesa is distributed in the hope that it will be useful,                 *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with Inovesa.  If not, see <http://www.gnu.org/licenses/>.           *
 ******************************************************************************/

#ifndef MESSAGESTRINGS_HPP
#define MESSAGESTRINGS_HPP

#include <iomanip>
#include <memory>
#include <sstream>


#include "PS/PhaseSpace.hpp"

namespace vfps
{

const std::string copyright_notice() noexcept;

/**
 * @brief inovesa_version
 * @param verbose
 * @return
 *
 * For branches leading to a release and for releases,
 * external applications rely on the format of the output
 * to determine the Inovesa feature level.
 * So, the string will always begin with "v{major}.{minor}"
 * followed by either ".{fix}" for releases
 * or " {descriptor}" for pre-releas versions.
 * Development versions do not have to follow this convention.
 */
const std::string inovesa_version(const bool verbose=false);

const std::string status_string(std::shared_ptr<PhaseSpace> ps,
                                float roatation,
                                float total_rotations);

} // namespace vfps

#endif // MESSAGESTRINGS_HPP
