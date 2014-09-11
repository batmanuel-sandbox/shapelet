// -*- lsst-c++ -*-
/*
 * LSST Data Management System
 * Copyright 2008-2014 LSST Corporation.
 *
 * This product includes software developed by the
 * LSST Project (http://www.lsst.org/).
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
 * see <http://www.lsstcorp.org/LegalNotices/>.
 */

#include <cmath>
#include "boost/make_shared.hpp"
#include "ndarray/eigen.h"

#include "lsst/shapelet/MatrixBuilder.h"
#include "lsst/shapelet/MultiShapeletBasis.h"
#include "lsst/shapelet/GaussHermiteConvolution.h"

namespace lsst { namespace shapelet {

namespace {

template <typename T>
Eigen::Map< Eigen::Array<T,Eigen::Dynamic,Eigen::Dynamic> > makeView(
    T * & workspace, int rows, int cols
) {
    Eigen::Map< Eigen::Array<T,Eigen::Dynamic,Eigen::Dynamic> > v(workspace, rows, cols);
    workspace += rows*cols;
    return v;
}

template <typename T>
Eigen::Map< Eigen::Array<T,Eigen::Dynamic,1> > makeView(
    T * & workspace, int size
) {
    Eigen::Map< Eigen::Array<T,Eigen::Dynamic,1> > v(workspace, size);
    workspace += size;
    return v;
}

template <typename T>
class EllipseHelper {
public:

    static int computeWorkspace(int dataSize) { return sizeof(T) * dataSize * 2; }

    explicit EllipseHelper(T * & workspace, int dataSize) :
        detFactor(1.0),
        xt(makeView(workspace, dataSize)),
        yt(makeView(workspace, dataSize))
    {}

    void readEllipse(
        ndarray::Array<T const,1,1> const & x,
        ndarray::Array<T const,1,1> const & y,
        afw::geom::ellipses::Ellipse const & ellipse
    ) {
        afw::geom::AffineTransform transform = ellipse.getGridTransform();
        xt = transform[afw::geom::AffineTransform::XX] * x.asEigen<Eigen::ArrayXpr>()
            + transform[afw::geom::AffineTransform::XY] * y.asEigen<Eigen::ArrayXpr>()
            + transform[afw::geom::AffineTransform::X];
        yt = transform[afw::geom::AffineTransform::YX] * x.asEigen<Eigen::ArrayXpr>()
            + transform[afw::geom::AffineTransform::YY] * y.asEigen<Eigen::ArrayXpr>()
            + transform[afw::geom::AffineTransform::Y];
        detFactor = transform.getLinear().computeDeterminant();
    }

    void scale(double factor) {
        xt.asEigen() /= factor;
        yt.asEigen() /= factor;
    }

    T detFactor;
    Eigen::Map< Eigen::Array<T,Eigen::Dynamic,1> > xt;
    Eigen::Map< Eigen::Array<T,Eigen::Dynamic,1> > yt;
};

template <typename T>
class GaussianHelper {
public:

    void apply(
        EllipseHelper<T> const & ellipseHelper,
        ndarray::Array<T,1,1> const & output
    ) {
        static T const NORM = 1.0 / std::sqrt(M_PI); // normalization to match shapelets
        // TODO: check that rowwise().squaredNorm() is optimized as well as explicitly writing it as
        // coeffwise array operations (may depend on whether we transpose xy).
        output.template asEigen<Eigen::ArrayXpr>() +=
            (-0.5*ellipseHelper.xyt.rowwise().squaredNorm()).array().exp()
            * ellipseHelper.detFactor
            * NORM;
    }

};

template <typename T>
class ShapeletHelper {
public:

    static int computeWorkspace(int dataSize, int order) {
        return sizeof(T) * dataSize * (1 + 2*(order + 1));
    }

    explicit ShapeletHelper(T *& workspace, int dataSize, int order) :
        _order(order),
        _expWorkspace(makeView(workspace, dataSize)),
        _xWorkspace(makeView(workspace, maxOrder + 1, dataSize)),
        _yWorkspace(makeView(workspace, maxOrder + 1, dataSize))
    {}

    void apply(
        EllipseHelper<T> const & ellipseHelper,
        ndarray::Array<T,2,-1> const & output
    ) {
        _expWorkspace =
            (-0.5*ellipseHelper.xyt.rowwise().squaredNorm()).array().exp() * ellipseHelper.detFactor;
        _fillHermite1d(_xWorkspace, ellipseHelper.xyt.col(0).array());
        _fillHermite1d(_yWorkspace, ellipseHelper.xyt.col(1).array());
        ndarray::EigenView<T,2,-1,Eigen::ArrayXpr> view(output);
        for (PackedIndex i; i.getOrder() <= _order; ++i) {
            view.col(i.getIndex()) += _expWorkspace*_xWorkspace.col(i.getX())*_yWorkspace.col(i.getY());
        }
    }

    int getMaxOrder() const { return _maxOrder; }

private:

    template <typename CoordArray>
    void _fillHermite1d(
        Eigen::Map<Eigen::Array<T,Eigen::Dynamic,Eigen::Dynamic> > & workspace,
        CoordArray const & coord
    ) {
        if (workspace.cols() > 0) {
            workspace.col(0).setConstant(BASIS_NORMALIZATION);
        }
        if (workspace.cols() > 1) {
            workspace.col(1) = intSqrt(2) * coord * workspace.col(0);
        }
        for (int j = 2; j <= _order; ++j) {
            workspace.col(j) = rationalSqrt(2, j) * coord * workspace.col(j-1)
                - rationalSqrt(j - 1, j) * workspace.col(j-2);
        }
    }

    int _maxOrder;
    Eigen::Map< Eigen::Array<T,Eigen::Dynamic,1> > _expWorkspace;
    Eigen::Map< Eigen::Array<T,Eigen::Dynamic,Eigen::Dynamic> > _xWorkspace;
    Eigen::Map< Eigen::Array<T,Eigen::Dynamic,Eigen::Dynamic> > _yWorkspace;
};

template <typename T>
class GaussianMatrixBuilder : public MatrixBuilder<T> {
public:

    GaussianMatrixBuilder(
        ndarray::Array<T const,1,1> const & x,
        ndarray::Array<T const,1,1> const & y
    ) : MatrixBuilder<T>(x, y, 1),
        _ellipseHelper(this->getDataSize())
    {}

    virtual void apply(
        ndarray::Array<T,2,-1> const & output,
        afw::geom::ellipses::Ellipse const & ellipse
    ) const {
        output.deep() = 0.0;
        _ellipseHelper.readEllipse(this->_xy, ellipse);
        _gaussianHelper.apply(_ellipseHelper, output.transpose()[0]);
    }

private:
    mutable EllipseHelper<T> _ellipseHelper;
    mutable GaussianHelper<T> _gaussianHelper;
};

template <typename T>
class ConvolvedGaussianMatrixBuilder : public MatrixBuilder<T> {
public:

    ConvolvedGaussianMatrixBuilder(
        ndarray::Array<T const,1,1> const & x,
        ndarray::Array<T const,1,1> const & y,
        afw::geom::ellipses::Ellipse const & psfEllipse,
        double psfCoefficient
    ) : MatrixBuilder<T>(x, y, 1),
        _ellipseHelper(this->getDataSize()),
        _psfEllipse(psfEllipse),
        _psfCoefficient(psfCoefficient)
    {}

    virtual void apply(
        ndarray::Array<T,2,-1> const & output,
        afw::geom::ellipses::Ellipse const & ellipse
    ) const {
        output.deep() = 0.0;
        _ellipseHelper.readEllipse(this->_xy, ellipse.convolve(_psfEllipse));
        _gaussianHelper.apply(_ellipseHelper, output.transpose()[0]);
        output.asEigen() *= _psfCoefficient / ShapeletFunction::FLUX_FACTOR;
    }

private:
    mutable EllipseHelper<T> _ellipseHelper;
    mutable GaussianHelper<T> _gaussianHelper;
    afw::geom::ellipses::Ellipse _psfEllipse;
    double _psfCoefficient;
};

template <typename T>
class ShapeletMatrixBuilder : public MatrixBuilder<T> {
public:

    ShapeletMatrixBuilder(
        ndarray::Array<T const,1,1> const & x,
        ndarray::Array<T const,1,1> const & y,
        int order
    ) : MatrixBuilder<T>(x, y, computeSize(order)),
        _order(order),
        _ellipseHelper(this->getDataSize()),
        _shapeletHelper(this->getDataSize(), order)
    {}

    virtual void apply(
        ndarray::Array<T,2,-1> const & output,
        afw::geom::ellipses::Ellipse const & ellipse
    ) const {
        output.deep() = 0.0;
        _ellipseHelper.readEllipse(this->_xy, ellipse);
        _shapeletHelper.apply(_ellipseHelper, output, _order);
    }

private:
    int const _order;
    mutable EllipseHelper<T> _ellipseHelper;
    mutable ShapeletHelper<T> _shapeletHelper;
};

template <typename T>
class ConvolvedShapeletMatrixBuilder : public MatrixBuilder<T> {
public:

    ConvolvedShapeletMatrixBuilder(
        ndarray::Array<T const,1,1> const & x,
        ndarray::Array<T const,1,1> const & y,
        ShapeletFunction const & psf,
        int order
    ) : MatrixBuilder<T>(x, y, computeSize(order)),
        _convolution(order, psf),
        _convolutionWorkspace(ndarray::allocate(this->getDataSize(), _convolution.getRowOrder())),
        _ellipseHelper(this->getDataSize()),
        _shapeletHelper(this->getDataSize(), _convolution.getRowOrder())
    {}

    virtual void apply(
        ndarray::Array<T,2,-1> const & output,
        afw::geom::ellipses::Ellipse const & ellipse
    ) const {
        _convolutionWorkspace.deep() = 0.0;
        afw::geom::ellipses::Ellipse convolvedEllipse(ellipse);
        ndarray::Array<double const,2,2> convolutionMatrix = _convolution.evaluate(convolvedEllipse);
        _ellipseHelper.readEllipse(this->_xy, convolvedEllipse);
        _shapeletHelper.apply(_ellipseHelper, _convolutionWorkspace, _convolution.getColOrder());
        output.asEigen() = _convolutionWorkspace.asEigen() * convolutionMatrix.asEigen().cast<T>();
    }

private:
    GaussHermiteConvolution _convolution;
    ndarray::Array<T,2,-1> _convolutionWorkspace;
    mutable EllipseHelper<T> _ellipseHelper;
    mutable ShapeletHelper<T> _shapeletHelper;
};

template <typename T>
class MultiShapeletMatrixBuilder0 : public MatrixBuilder<T> {
public:

    MultiShapeletMatrixBuilder0(
        ndarray::Array<T const,1,1> const & x,
        ndarray::Array<T const,1,1> const & y,
        MultiShapeletBasis const & basis
    ) : MatrixBuilder<T>(x, y, basis.getSize()),
        _basis(basis),
        _ellipseHelper(this->getDataSize()),
        _shapeletHelper(this->getDataSize(), _computeMaxOrder()),
        _basisWorkspace(ndarray::allocate(this->getDataSize(), computeSize(_shapeletHelper.getMaxOrder())))
    {}

    virtual void apply(
        ndarray::Array<T,2,-1> const & output,
        afw::geom::ellipses::Ellipse const & ellipse
    ) const {
        output.deep() = 0.0;
        _ellipseHelper.readEllipse(this->_xy, ellipse);
        double lastRadius = 1.0;
        for (MultiShapeletBasis::Iterator iter = _basis.begin(); iter != _basis.end(); ++iter) {
            _ellipseHelper.scale(iter->getRadius() / lastRadius);
            ndarray::Array<T,2,1> view = _basisWorkspace[ndarray::view()(0, computeSize(iter->getOrder()))];
            view.deep() = 0.0;
            _shapeletHelper.apply(_ellipseHelper, view, iter->getOrder());
            output.asEigen() += view.asEigen() * iter->getMatrix().asEigen().cast<T>();
            lastRadius = iter->getRadius();
        }
    }

private:

    int _computeMaxOrder() const {
        int maxOrder = 0;
        for (MultiShapeletBasis::Iterator iter = _basis.begin(); iter != _basis.end(); ++iter) {
            maxOrder = std::max(maxOrder, iter->getOrder());
        }
        return maxOrder;
    }

    MultiShapeletBasis _basis;
    mutable EllipseHelper<T> _ellipseHelper;
    mutable ShapeletHelper<T> _shapeletHelper;
    ndarray::Array<T,2,-1> _basisWorkspace;
    mutable Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> _convolutionWorkspace;
};

class ConvolvedMultiShapeletMatrixBuilderComponent {
public:

    explicit ConvolvedMultiShapeletMatrixBuilderComponent(
        MultiShapeletBasisComponent const & component,
        ShapeletFunction const & psf
    ) : convolution(component.getOrder(), psf),
        radius(component.getRadius()),
        matrix(component.getMatrix())
    {}

    int getRowSize() const { return computeSize(convolution.getRowOrder()); }

    int getColSize() const { return computeSize(convolution.getColOrder()); }

    GaussHermiteConvolution convolution;
    double radius;
    ndarray::Array<double const,2,2> matrix;
};

template <typename T>
class ConvolvedMultiShapeletMatrixBuilder : public MatrixBuilder<T> {
public:

    typedef ConvolvedMultiShapeletMatrixBuilderComponent Component;

    ConvolvedMultiShapeletMatrixBuilder(
        ndarray::Array<T const,1,1> const & x,
        ndarray::Array<T const,1,1> const & y,
        MultiShapeletFunction const & psf,
        MultiShapeletBasis const & basis
    ) : MatrixBuilder<T>(x, y, basis.getSize()),
        _ellipseHelper(this->getDataSize()),
        _shapeletHelper() // need to wait until after we initialize components to truly initialize this
    {
        int maxRowOrder = 0;
        int maxColOrder = 0;
        _components.reserve(psf.getElements().size() * basis.getComponentCount());
        for (MultiShapeletBasis::Iterator basisIter = basis.begin(); basisIter != basis.end(); ++basisIter) {
            for (
                MultiShapeletFunction::ElementList::const_iterator psfIter = psf.getElements().begin();
                psfIter != psf.getElements().end();
                ++psfIter
            ) {
                _components.push_back(Component(*basisIter, *psfIter));
                maxRowOrder = std::max(maxRowOrder, _components.back().convolution.getRowOrder());
                maxColOrder = std::max(maxColOrder, _components.back().convolution.getColOrder());
            }
        }
        _shapeletHelper = ShapeletHelper<T>(this->getDataSize(), maxRowOrder);
        _basisWorkspace = ndarray::allocate(this->getDataSize(), maxRowOrder);
        _convolutionWorkspace = ndarray::allocate(maxRowOrder, maxColOrder);
    }

    virtual void apply(
        ndarray::Array<T,2,-1> const & output,
        afw::geom::ellipses::Ellipse const & ellipse
    ) const {
        output.deep() = 0.0;
        for (
            std::vector<Component>::const_iterator iter = _components.begin();
            iter != _components.end();
            ++iter
        ) {
            afw::geom::ellipses::Ellipse tmpEllipse(ellipse);
            tmpEllipse.getCore().scale(iter->radius);

            // convolve the ellipse and create a matrix which convolves  coefficients
            ndarray::Array<double const,2,2> convolutionMatrix = iter->convolution.evaluate(tmpEllipse);
            _ellipseHelper.readEllipse(this->_xy, tmpEllipse);

            // evaluate simple shapelet basis
            ndarray::Array<T,2,-1> basisView = _basisWorkspace[ndarray::view()(0, iter->getRowSize())];
            basisView.deep() = 0.0;
            _shapeletHelper.apply(_ellipseHelper, basisView, iter->convolution.getColOrder());

            // compute the product of the convolution matrix with the basis matrix
            ndarray::Array<T,2,-1> convolutionView
                = _convolutionWorkspace[ndarray::view(0, iter->getRowSize())];
            convolutionView.asEigen() = (convolutionMatrix.asEigen()*iter->matrix.asEigen()).cast<T>();

            output.asEigen() += basisView.asEigen() * convolutionView.asEigen();
        }
    }

private:


    mutable EllipseHelper<T> _ellipseHelper;
    mutable ShapeletHelper<T> _shapeletHelper;
    std::vector<Component> _components;
    ndarray::Array<T,2,-1> _basisWorkspace;
    ndarray::Array<T,2,-1> _convolutionWorkspace;
};

} // anonymous

template <typename T>
MatrixBuilder<T>::MatrixBuilder(
    ndarray::Array<T const,1,1> const & x,
    ndarray::Array<T const,1,1> const & y,
    int basisSize
) : _basisSize(basisSize), _xy(x.template getSize<0>(), 2) {
    LSST_THROW_IF_NE(
        x.template getSize<0>(), y.template getSize<0>(), pex::exceptions::LengthError,
        "Size of x array (%d) does not match size of y array (%d)"
    );
    _xy.col(0) = x.asEigen();
    _xy.col(1) = y.asEigen();
}

template <typename T>
PTR(MatrixBuilder<T>) makeMatrixBuilder(
    ndarray::Array<T const,1,1> const & x,
    ndarray::Array<T const,1,1> const & y,
    int order
) {
    if (order == 0) {
        return boost::make_shared< GaussianMatrixBuilder<T> >(x, y);
    } else {
        return boost::make_shared< ShapeletMatrixBuilder<T> >(x, y, order);
    }
}
template <typename T>
PTR(MatrixBuilder<T>) makeMatrixBuilder(
    ndarray::Array<T const,1,1> const & x,
    ndarray::Array<T const,1,1> const & y,
    ShapeletFunction const & psf,
    int order
) {
    if (order == 0 && psf.getOrder() == 0) {
        return boost::make_shared< ConvolvedGaussianMatrixBuilder<T> >(
            x, y, psf.getEllipse(), psf.getCoefficients()[0]
        );
    } else {
        return boost::make_shared< ConvolvedShapeletMatrixBuilder<T> >(x, y, psf, order);
    }
}

template <typename T>
PTR(MatrixBuilder<T>) makeMatrixBuilder(
    ndarray::Array<T const,1,1> const & x,
    ndarray::Array<T const,1,1> const & y,
    MultiShapeletFunction const & psf,
    int order
) {
    if (psf.getElements().size() == 1u) {
        return makeMatrixBuilder(x, y, psf.getElements().front(), order);
    } else {
        throw pex::exceptions::LogicError("Not implemented");
    }
}

template <typename T>
PTR(MatrixBuilder<T>) makeMatrixBuilder(
    ndarray::Array<T const,1,1> const & x,
    ndarray::Array<T const,1,1> const & y,
    MultiShapeletBasis const & basis
) {
    return boost::make_shared< MultiShapeletMatrixBuilder0<T> >(x, y, basis);
}

template <typename T>
PTR(MatrixBuilder<T>) makeMatrixBuilder(
    ndarray::Array<T const,1,1> const & x,
    ndarray::Array<T const,1,1> const & y,
    MultiShapeletFunction const & psf,
    MultiShapeletBasis const & basis
) {
    throw pex::exceptions::LogicError("Not implemented");
}

#define INSTANTIATE(T)                                      \
    template class MatrixBuilder<T>;                        \
    template class GaussianMatrixBuilder<T>;                \
    template class ConvolvedGaussianMatrixBuilder<T>;       \
    template class ShapeletMatrixBuilder<T>;                \
    template class ConvolvedShapeletMatrixBuilder<T>;       \
    template class MultiShapeletMatrixBuilder0<T>;          \
    template class ConvolvedMultiShapeletMatrixBuilder<T>;  \
    template PTR(MatrixBuilder<T>) makeMatrixBuilder(       \
        ndarray::Array<T const,1,1> const &,                \
        ndarray::Array<T const,1,1> const &,                \
        int order                                           \
    );                                                      \
    PTR(MatrixBuilder<T>) makeMatrixBuilder(                \
        ndarray::Array<T const,1,1> const &,                \
        ndarray::Array<T const,1,1> const &,                \
        ShapeletFunction const &,                           \
        int order                                           \
    );                                                      \
    PTR(MatrixBuilder<T>) makeMatrixBuilder(                \
        ndarray::Array<T const,1,1> const &,                \
        ndarray::Array<T const,1,1> const &,                \
        MultiShapeletFunction const &,                      \
        int order                                           \
    );                                                      \
    PTR(MatrixBuilder<T>) makeMatrixBuilder(                \
        ndarray::Array<T const,1,1> const &,                \
        ndarray::Array<T const,1,1> const &,                \
        MultiShapeletBasis const &                          \
    );                                                      \
    PTR(MatrixBuilder<T>) makeMatrixBuilder(                \
        ndarray::Array<T const,1,1> const &,                \
        ndarray::Array<T const,1,1> const &,                \
        MultiShapeletFunction const &,                      \
        MultiShapeletBasis const &                          \
    )

INSTANTIATE(float);
INSTANTIATE(double);

}} // namespace lsst::shapelet
