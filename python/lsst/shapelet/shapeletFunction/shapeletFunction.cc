/*
 * LSST Data Management System
 *
 * This product includes software developed by the
 * LSST Project (http://www.lsst.org/).
 * See the COPYRIGHT file
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the LSST License Statement and
 * the GNU General Public License along with this program.  If not,
 * see <https://www.lsstcorp.org/LegalNotices/>.
 */
#include "pybind11/pybind11.h"

#include "numpy/arrayobject.h"
#include "ndarray/pybind11.h"

#include "lsst/shapelet/ShapeletFunction.h"

namespace py = pybind11;
using namespace pybind11::literals;

namespace lsst {
namespace shapelet {

namespace {

template <typename PixelT, typename PyClass>
void declareShapeletFunctionEvaluatorAddToImageTypes(PyClass & cls){
    cls.def(
            "addToImage", (void (ShapeletFunctionEvaluator::*)(ndarray::Array<PixelT, 2, 1> const &,
                                                               afw::geom::Point2I const &) const) &
                                  ShapeletFunctionEvaluator::addToImage<PixelT>,
            "array"_a, "xy0"_a = afw::geom::Point2D());
    cls.def(
            "addToImage", (void (ShapeletFunctionEvaluator::*)(afw::image::Image<PixelT> &) const) &
                                  ShapeletFunctionEvaluator::addToImage<PixelT>,
            "image"_a);
}

} // end anonymous namespace


PYBIND11_PLUGIN(shapeletFunction) {
    py::module::import("lsst.afw.geom");
    py::module::import("lsst.afw.image");

    py::module mod("shapeletFunction");

    if (_import_array() < 0) {
        PyErr_SetString(PyExc_ImportError, "numpy.core.multiarray failed to import");
        return nullptr;
    }

    py::class_<ShapeletFunction, std::shared_ptr<ShapeletFunction>> clsShapeletFunction(mod,
                                                                                        "ShapeletFunction");

    clsShapeletFunction.def_readonly_static("FLUX_FACTOR", &ShapeletFunction::FLUX_FACTOR);

    clsShapeletFunction.def(py::init<int, BasisTypeEnum>(), "order"_a, "basisType"_a);
    clsShapeletFunction.def(py::init<int, BasisTypeEnum, ndarray::Array<double, 1, 1> const &>(), "order"_a,
                            "basisType"_a, "coefficients"_a);
    clsShapeletFunction.def(py::init<int, BasisTypeEnum, double, afw::geom::Point2D const &>(), "order"_a,
                            "basisType"_a, "radius"_a, "center"_a = afw::geom::Point2D());
    clsShapeletFunction.def(py::init<int, BasisTypeEnum, double, afw::geom::Point2D const &,
                                     ndarray::Array<double, 1, 1> const &>(),
                            "order"_a, "basisType"_a, "radius"_a, "center"_a, "coefficients"_a);
    clsShapeletFunction.def(py::init<int, BasisTypeEnum, afw::geom::ellipses::Ellipse const &>(), "order"_a,
                            "basisType"_a, "ellipse"_a);
    clsShapeletFunction.def(py::init<int, BasisTypeEnum, afw::geom::ellipses::Ellipse const &,
                                     ndarray::Array<double const, 1, 1> const &>(),
                            "order"_a, "basisType"_a, "ellipse"_a, "coefficients"_a);
    clsShapeletFunction.def(py::init<ShapeletFunction>());

    clsShapeletFunction.def("getOrder", &ShapeletFunction::getOrder);
    clsShapeletFunction.def("getEllipse", (afw::geom::ellipses::Ellipse & (ShapeletFunction::*)()) &
                                                  ShapeletFunction::getEllipse,
                            py::return_value_policy::reference_internal);
    clsShapeletFunction.def("setEllipse", &ShapeletFunction::setEllipse);
    clsShapeletFunction.def("getBasisType", &ShapeletFunction::getBasisType);
    clsShapeletFunction.def("changeBasisType", &ShapeletFunction::changeBasisType);
    clsShapeletFunction.def("normalize", &ShapeletFunction::normalize, "value"_a = 1.0);
    clsShapeletFunction.def("getCoefficients", (ndarray::Array<double, 1, 1> const (ShapeletFunction::*)()) &
                                                       ShapeletFunction::getCoefficients);
    clsShapeletFunction.def("convolve", &ShapeletFunction::convolve);
    clsShapeletFunction.def("evaluate", &ShapeletFunction::evaluate);
    clsShapeletFunction.def("shiftInPlace", &ShapeletFunction::shiftInPlace);
    clsShapeletFunction.def("transformInPlace", &ShapeletFunction::transformInPlace);

    py::class_<ShapeletFunctionEvaluator, std::shared_ptr<ShapeletFunctionEvaluator>>
            clsShapeletFunctionEvaluator(mod, "ShapeletFunctionEvaluator");

    clsShapeletFunctionEvaluator.def(py::init<ShapeletFunction const &>(), "function"_a);

    clsShapeletFunctionEvaluator.def("__call__",
                                     (double (ShapeletFunctionEvaluator::*)(double, double) const) &
                                             ShapeletFunctionEvaluator::operator());
    clsShapeletFunctionEvaluator.def(
            "__call__", (double (ShapeletFunctionEvaluator::*)(afw::geom::Point2D const &) const) &
                                ShapeletFunctionEvaluator::operator());
    clsShapeletFunctionEvaluator.def(
            "__call__", (double (ShapeletFunctionEvaluator::*)(afw::geom::Extent2D const &) const) &
                                ShapeletFunctionEvaluator::operator());
    clsShapeletFunctionEvaluator.def("__call__", (ndarray::Array<double, 1, 1> (ShapeletFunctionEvaluator::*)(
                                                         ndarray::Array<double const, 1> const &,
                                                         ndarray::Array<double const, 1> const &) const) &
                                                         ShapeletFunctionEvaluator::operator());

    declareShapeletFunctionEvaluatorAddToImageTypes<float>(clsShapeletFunctionEvaluator);
    declareShapeletFunctionEvaluatorAddToImageTypes<double>(clsShapeletFunctionEvaluator);

    clsShapeletFunctionEvaluator.def("integrate", &ShapeletFunctionEvaluator::integrate);
    clsShapeletFunctionEvaluator.def("computeMoments", &ShapeletFunctionEvaluator::computeMoments);
    clsShapeletFunctionEvaluator.def("update", &ShapeletFunctionEvaluator::update);

    return mod.ptr();
}

}  // shapelet
}  // lsst
