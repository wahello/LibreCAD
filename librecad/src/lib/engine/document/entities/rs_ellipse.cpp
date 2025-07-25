﻿/****************************************************************************
**
** This file is part of the LibreCAD project, a 2D CAD program
**
** Copyright (C) 2011-2015 Dongxu Li (dongxuli2011@gmail.com)
** Copyright (C) 2010 R. van Twisk (librecad@rvt.dds.nl)
** Copyright (C) 2001-2003 RibbonSoft. All rights reserved.
**
**
** This file may be distributed and/or modified under the terms of the
** GNU General Public License version 2 as published by the Free Software
** Foundation and appearing in the file gpl-2.0.txt included in the
** packaging of this file.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
**
** This copyright notice MUST APPEAR in all copies of the script!
**
**********************************************************************/

#include "rs_ellipse.h"

#include "lc_quadratic.h"
#include "lc_rect.h"
#include "rs_circle.h"
#include "rs_debug.h"
#include "rs_entitycontainer.h"
#include "rs_information.h"
#include "rs_line.h"
#include "rs_math.h"
#include "rs_painter.h"

#ifdef EMU_C99
#include "emu_c99.h" /* C99 math */
#endif
// Workaround for Qt bug: https://bugreports.qt-project.org/browse/QTBUG-22829
// TODO: the Q_MOC_RUN detection shouldn't be necessary after this Qt bug is resolved
#ifndef Q_MOC_RUN
#include <boost/version.hpp>
#include <boost/math/tools/roots.hpp>
#include <boost/math/special_functions/ellint_2.hpp>
#if BOOST_VERSION > 104500
#include <boost/tuple/tuple.hpp>
#endif
#endif

namespace{
//functor to solve for distance, used by snapDistance
class EllipseDistanceFunctor
{
public:
    EllipseDistanceFunctor(RS_Ellipse const& ellipse, double const& target) :
		distance{target}
	  , e{ellipse}
      , ra{e.getMajorRadius()}
      , k2{1. - e.getRatio() * e.getRatio()}
	  , k2ra{k2 * ra}
	{
	}
	void setDistance(const double& target){
		distance=target;
	}
#if BOOST_VERSION > 104500
    boost::tuples::tuple<double, double, double> operator()(double const& z) const {
#else
	boost::fusion::tuple<double, double, double> operator()(double const& z) const {
#endif
        double const cz=std::cos(z);
        double const sz=std::sin(z);
        //delta amplitude
        double const d=std::sqrt(1-k2*sz*sz);
        // return f(x), f'(x) and f''(x)
#if BOOST_VERSION > 104500
        return boost::tuples::make_tuple(
#else
        return boost::fusion::make_tuple(
#endif
                    e.getEllipseLength(z) - distance,
                    ra * d,
                    k2ra * sz * cz / d );
        }

private:
	double distance;
    RS_Ellipse const& e;
	const double ra;
	const double k2;
	const double k2ra;
};

/**
 * @brief getNearestDistHelper find end point after trimmed by amount
 * @param e ellipse which is not reversed, assume ratio (a/b) >= 1
 * @param trimAmount the length of the trimmed is increased by this amount
 * @param coord current mouse position
 * @param dist if this pointer is not nullptr, save the distance from the new
 * end point to mouse position coord
 * @return the new end point of the trimmed. Only one end of the entity is
 *  trimmed
 */
RS_Vector getNearestDistHelper(RS_Ellipse const& e,
							   double trimAmount,
							   RS_Vector const& coord,
							   double* dist = nullptr)
{
	double const x1 = e.getAngle1();

	double const guess= x1 + M_PI;
	int const digits=std::numeric_limits<double>::digits;

	double const wholeLength = e.getEllipseLength(0, 0); // start/end angle 0 is used for whole ellipses

	double trimmed = e.getLength() + trimAmount;

	// choose the end to trim by the mouse position coord
	bool const trimEnd = coord.squaredTo(e.getStartpoint()) <= coord.squaredTo(e.getEndpoint());

	if (trimEnd)
		trimmed = trimAmount > 0 ? wholeLength - trimAmount : - trimAmount;

	//solve equation of the distance by second order newton_raphson
    EllipseDistanceFunctor X{e, trimmed};
	using namespace boost::math::tools;
	double const sol =
			halley_iterate<EllipseDistanceFunctor,double>(X,
														  guess,
														  x1,
														  x1 + 2 * M_PI - RS_TOLERANCE_ANGLE,
														  digits);

	RS_Vector const vp = e.getEllipsePoint(sol);
    if (dist)
        *dist = vp.distanceTo(coord);
    return vp;
}

/**
 * @brief The ClosestElliptic class: find the closest point on an ellipse for a given point.
 * Intended for ellipses with small eccentricities.
 * Algorithm: Newton-Raphson
 * Added for issue #1653
 */
class ClosestEllipticPoint {
public:
    ClosestEllipticPoint(double a, double b, const RS_Vector& point):
        m_point{point}
      , c2{b * b - a * a}
      , ax2{2.*a*point.x}
      , by2{2.*b*point.y}
    {}

    // The elliptic angle of the closest point on ellipse.
    double getTheta() const
    {
        double theta = std::atan2(m_point.y, m_point.x);
        // find the zero point of the first order derivative by Newton-Raphson
        // the convergence should be good: maximum 16 recursions
        for (short i=0; i<16; ++i) {
            // The first and second derivatives over theta
            double d1 = ds2D1(theta);
            double d2 = ds2D2(theta);
            if (std::abs(d2) < RS_TOLERANCE || std::abs(d1) < RS_TOLERANCE)
                break;
            // Newton-Raphson
            theta -= d1/d2;
        }
        return theta;
    }

private:

    // The first order derivative of ds2=dx^2+dy^2 over theta
    double ds2D1(double t) const
    {
        using namespace std;
        return c2*sin(2.*t) + ax2*sin(t) - by2*cos(t);
    }

    // The second order derivative of ds2=dx^2+dy^2 over theta
    double ds2D2(double t) const
    {
        using namespace std;
        return 2.*c2*cos(2.*t) + ax2*cos(t) + by2*sin(t);
    }

    RS_Vector m_point{};
    double c2=0.;
    double ax2=0.;
    double by2=0.;
};

/**
 * @brief The EllipseBorderHelper class a helper class to avoid infinite loop due to calculateBorders()
 *        The only difference from RS_Ellipse is a no-op calculateBorders() method
 */
class EllipseBorderHelper: public RS_Ellipse {
public:
    EllipseBorderHelper(const RS_Ellipse& ellipse):
        RS_Ellipse(ellipse)
    {}

    // No-op to avoid infinite loop in RS_Ellipse::calculateBorders()
    void calculateBorders() override
    {}
};

}

std::ostream& operator << (std::ostream& os, const RS_EllipseData& ed) {
	os << "(" << ed.center <<
		  " " << ed.majorP <<
		  " " << ed.ratio <<
		  " " << ed.angle1 <<
		  "," << ed.angle2 <<
		  ")";
	return os;
}

/**
 * Constructor.
 */
RS_Ellipse::RS_Ellipse(RS_EntityContainer* parent,
                       const RS_EllipseData& d)
	:LC_CachedLengthEntity(parent)
	,data(d) {
    //calculateEndpoints();
    calculateBorders();
}

RS_Entity* RS_Ellipse::clone() const {
	auto* e = new RS_Ellipse(*this);
	return e;
}

/**
 * Calculates the boundary box of this ellipse.
  * @author Dongxu Li
 */
void RS_Ellipse::calculateBorders() {

#ifndef EMU_C99
    using std::isnormal;
#endif
    if (std::abs(data.angle1) < RS_TOLERANCE_ANGLE && (std::abs(data.angle2) < RS_TOLERANCE_ANGLE)){
        data.angle1 = 0;
        data.angle2 = 0;
    }
    data.isArc = isnormal(data.angle1) || isnormal(data.angle2);

    LC_Rect boundingBox = isEllipticArc() ? LC_Rect{ getStartpoint(), getEndpoint() } : LC_Rect{};

    // x-range extremes are at this direction and its opposite, relative to the ellipse center
    const RS_Vector vpx{ getMajorP().x, -getRatio()*getMajorP().y };
    mergeBoundingBox(boundingBox, vpx);

    // y-range extremes are at this direction and its opposite, relative to the ellipse center
    const RS_Vector vpy{ getMajorP().y, getRatio()*getMajorP().x };
    mergeBoundingBox(boundingBox, vpy);

    minV = boundingBox.minP();
    maxV = boundingBox.maxP();

    data.angleDegrees = RS_Math::rad2deg(getAngle());
    data.startAngleDegrees = RS_Math::rad2deg(data.reversed ? data.angle2 : data.angle1);
    data.otherAngleDegrees = RS_Math::rad2deg(data.reversed ? data.angle1 : data.angle2);
    data.angularLength = RS_Math::rad2deg(RS_Math::getAngleDifference(data.angle1, data.angle2, data.reversed));
    if (std::abs(data.angularLength) < RS_TOLERANCE_ANGLE) {
        // check whether angles are via period
        if (RS_Math::getPeriodsCount(data.angle1, data.angle2, data.reversed) != 0) {
            data.angularLength = 360; // in degrees
        }
    }

    updateLength();
}

void RS_Ellipse::mergeBoundingBox(LC_Rect& boundingBox, const RS_Vector& direction)
{
    const double angle = direction.angle();
    // Test the given direction and its opposite
    for(double a: {angle, angle + M_PI})
        if(RS_Math::isAngleBetween(a, getAngle1(), getAngle2(), isReversed()))
            boundingBox = boundingBox.merge(getEllipsePoint(a));
}


/**
  * return the foci of ellipse
  *
  * @author Dongxu Li
  */

RS_VectorSolutions RS_Ellipse::getFoci() const {
    RS_Ellipse e=*this;
    if(getRatio()>1.)
        e.switchMajorMinor();
    RS_Vector vp(e.getMajorP()*sqrt(1.-e.getRatio()*e.getRatio()));
    return RS_VectorSolutions({getCenter()+vp, getCenter()-vp});
}

RS_VectorSolutions RS_Ellipse::getRefPoints() const
{
    RS_VectorSolutions ret;
    if(isEllipticArc()){
        //no start/end point for whole ellipse
        ret.push_back(getStartpoint());
        ret.push_back(getEndpoint());
    }
    ret.push_back(data.center);
    ret.push_back(getFoci());
    ret.push_back(getMajorPoint());
    ret.push_back(getMinorPoint());
    return ret;
}



RS_Vector RS_Ellipse::getNearestEndpoint(const RS_Vector& coord, double* dist)const {
    if (!isEllipticArc())
        return RS_Vector{false};

    RS_Vector startpoint = getStartpoint();
    RS_Vector endpoint = getEndpoint();

    double dist1 = (startpoint-coord).squared();
    double dist2 = (endpoint-coord).squared();

    if (dist2<dist1) {
		if (dist) {
            *dist = sqrt(dist2);
        }
        return endpoint;
    } else {
		if (dist) {
            *dist = sqrt(dist1);
        }
        return startpoint;
    }
}

/**
  *find the tangential points from a given point, i.e., the tangent lines should pass
  * the given point and tangential points
  *
  * \author Dongxu Li
  */
RS_VectorSolutions RS_Ellipse::getTangentPoint(const RS_Vector& point) const {
    RS_Vector point2(point);
    point2.move(-getCenter());
    RS_Vector aV(-getAngle());
    point2.rotate(aV);
    RS_VectorSolutions sol;
    double a=getMajorRadius();
    if(a<RS_TOLERANCE || getRatio()<RS_TOLERANCE) return sol;
	RS_Circle c(nullptr, RS_CircleData(RS_Vector(0.,0.),a));
    point2.y /=getRatio();
    sol=c.getTangentPoint(point2);
    sol.scale(RS_Vector(1.,getRatio()));
    aV.y *= -1.;
    sol.rotate(aV);
    sol.move(getCenter());
    return sol;
}

RS_Vector RS_Ellipse::getTangentDirection(const RS_Vector &point) const {
    RS_Vector vp = point - getCenter();
    RS_Vector aV{-getAngle()};
    vp.rotate(aV);
    vp.y /= getRatio();
    double a = getMajorRadius();
    if (a < RS_TOLERANCE || getRatio() < RS_TOLERANCE)
        return {};
    RS_Circle c(nullptr, RS_CircleData(RS_Vector(0., 0.), a));
    RS_Vector direction = c.getTangentDirection(vp);
    direction.y *= getRatio();
    aV.y *= -1.;
    direction.rotate(aV);
    return isReversed() ? -direction : direction;
}

/**
  * find total length of the ellipse (arc)
  *
  * \author: Dongxu Li
  */
void RS_Ellipse::updateLength() {
    // EllipseBorderHelper class has a no-op calculateBorders() method
    EllipseBorderHelper e{*this};

    //switch major/minor axis, because we need the ratio smaller than one in getEllipseLength()
    if(e.getRatio()>1.)
        e.switchMajorMinor();

    // required to be not reversed in getEllipseLength()
    if(e.isReversed()) {
        std::swap(e.data.angle1, e.data.angle2);
        e.setReversed(false);
    }
    cachedLength = e.getEllipseLength(e.data.angle1,e.data.angle2);
}

/**
//Ellipse must have ratio<1, and not reversed
*@ x1, ellipse angle
*@ x2, ellipse angle
//@return the arc length between ellipse angle x1, x2
* \author Dongxu Li
**/
double RS_Ellipse::getEllipseLength(double x1, double x2) const{
    double a(getMajorRadius()),k(getRatio());
    k= std::sqrt(1-k*k);//elliptic modulus, or eccentricity
//    std::cout<<"1, angle1="<<x1/M_PI<<" angle2="<<x2/M_PI<<std::endl;
//    if(isReversed())  std::swap(x1,x2);
    x1=RS_Math::correctAngle(x1);
    x2=RS_Math::correctAngle(x2);
//    std::cout<<"2, angle1="<<x1/M_PI<<" angle2="<<x2/M_PI<<std::endl;
    if(x2 < x1+RS_TOLERANCE_ANGLE) x2 += 2.*M_PI;
    double ret = 0.;
//    std::cout<<"3, angle1="<<x1/M_PI<<" angle2="<<x2/M_PI<<std::endl;
    if( x2 >= M_PI) {
        // the complete elliptic integral
        ret=  (static_cast<int>((x2+RS_TOLERANCE_ANGLE)/M_PI) -
               (static_cast<int>((x1+RS_TOLERANCE_ANGLE)/M_PI)
                ))*2;
//        std::cout<<"Adding "<<ret<<" of E("<<k<<")\n";
        ret*=boost::math::ellint_2<double>(k);
    } else {
        ret=0.;
    }
    x1=std::fmod(x1,M_PI);
    x2=std::fmod(x2,M_PI);
    if( std::abs(x2-x1)>RS_TOLERANCE_ANGLE)  {
        ret += RS_Math::ellipticIntegral_2(k,x2)-RS_Math::ellipticIntegral_2(k,x1);
    }
    return a*ret;
}

/**
  * arc length from start point (angle1)
  */
double RS_Ellipse::getEllipseLength(double x2) const{
    return getEllipseLength(getAngle1(),x2);
}

/**
  * get the point on the ellipse arc and with arc distance from the start point
  * the distance is expected to be within 0 and getLength()
  * using Newton-Raphson from boost
  *
  *@author: Dongxu Li
  */

RS_Vector RS_Ellipse::getNearestDist(double distance,
                                     const RS_Vector& coord,
									 double* dist) const{
//    RS_DEBUG->print("RS_Ellipse::getNearestDist() begin\n");
    if( ! isEllipticArc() ) {
        // both angles being 0, whole ellipse
        // no end points for whole ellipse, therefore, no snap by distance from end points.
        return {};
    }
    RS_Ellipse e(nullptr,data);
    if(e.getRatio()>1.) e.switchMajorMinor();
    if(e.isReversed()) {
        std::swap(e.data.angle1,e.data.angle2);
        e.setReversed(false);
    }

    if(e.getMajorRadius() < RS_TOLERANCE)
        return {}; //ellipse too small

    if(getRatio()<RS_TOLERANCE) {
        //treat the ellipse as a line
        RS_Line line{e.minV,e.maxV};
        return line.getNearestDist(distance, coord, dist);
    }
    double x1=e.getAngle1();
    double x2=e.getAngle2();
    if(x2 < x1+RS_TOLERANCE_ANGLE) x2 += 2 * M_PI;
    double const l0=e.getEllipseLength(x1,x2); // the getEllipseLength() function only defined for proper e
//    distance=std::abs(distance);
    if(distance > l0+RS_TOLERANCE)
        return {}; // can not trim more than the current length
    if(distance > l0-RS_TOLERANCE)
        return getNearestEndpoint(coord,dist); // trim to zero length

    return getNearestDistHelper(e, distance, coord, dist);
}


/**
  * switch the major/minor axis naming
  *
  * \author: Dongxu Li
  */
bool RS_Ellipse::switchMajorMinor(void)
//switch naming of major/minor, return true if success
{
    if (std::abs(data.ratio) < RS_TOLERANCE)
        return false;
    RS_Vector vp_start=getStartpoint();
    RS_Vector vp_end=getEndpoint();
    RS_Vector vp=getMajorP();
    setMajorP(RS_Vector(- data.ratio*vp.y, data.ratio*vp.x)); //direction pi/2 relative to old MajorP;
    setRatio(1./data.ratio);
    if( isEllipticArc() )  {
        //only reset start/end points for ellipse arcs, i.e., angle1 angle2 are not both zero
        setAngle1(getEllipseAngle(vp_start));
        setAngle2(getEllipseAngle(vp_end));
    }
    calculateBorders();
    return true;
}

/**
 * @return Start point of the entity.
 */
RS_Vector  RS_Ellipse::getStartpoint() const {
    return isEllipticArc() ? getEllipsePoint(data.angle1) :  RS_Vector{false};
}

/**
 * @return End point of the entity.
 */
RS_Vector  RS_Ellipse::getEndpoint() const {
    return isEllipticArc() ? getEllipsePoint(data.angle2) :  RS_Vector{false};
}

/**
 * @return Ellipse point by ellipse angle
 */
RS_Vector  RS_Ellipse::getEllipsePoint(double a) const {
    RS_Vector point{a};
    double ra=getMajorRadius();
    point.scale(RS_Vector(ra,ra*getRatio()));
    point.rotate(getAngle());
    point.move(getCenter());
    return point;
}

/** \brief implemented using an analytical algorithm
* find nearest point on ellipse to a given point
*
* @author Dongxu Li <dongxuli2011@gmail.com>
*/
RS_Vector RS_Ellipse::getNearestPointOnEntity(const RS_Vector& coord,
        bool onEntity, double* dist, RS_Entity** entity)const{

    RS_DEBUG->print("RS_Ellipse::getNearestPointOnEntity");
    RS_Vector ret(false);

    if( !coord.valid ) {
        if (dist != nullptr)
            *dist=RS_MAXDOUBLE;
        return ret;

    }

    if (entity != nullptr) {
        *entity = const_cast<RS_Ellipse*>(this);
    }
    ret=coord;
    ret.move(-getCenter());
    ret.rotate(-getAngle());
    double x=ret.x,y=ret.y;
    double a=getMajorRadius();
    double b=a*getRatio();
    // the tangential direction at the nearest
    RS_Vector perpendicular{-ret.y, ret.x};
    //std::cout<<"(a= "<<a<<" b= "<<b<<" x= "<<x<<" y= "<<y<<" )\n";
    //std::cout<<"finding minimum for ("<<x<<"-"<<a<<"*cos(t))^2+("<<y<<"-"<<b<<"*sin(t))^2\n";
    double twoa2b2=2*(a*a-b*b);
    double twoax=2*a*x;
    double twoby=2*b*y;
    double a0=twoa2b2*twoa2b2;
    std::vector<double> ce(4,0.);
    std::vector<double> roots;

    //need to handle: a=b (i.e. a0=0); point close to the ellipse origin.
    if (a0 > RS_TOLERANCE && std::abs(getRatio() - 1.0) > RS_TOLERANCE && ret.squared() > RS_TOLERANCE2 ) {
        // a != b , ellipse
        ce[0]=-2.*twoax/twoa2b2;
        ce[1]= (twoax*twoax+twoby*twoby)/a0-1.;
        ce[2]= - ce[0];
        ce[3]= -twoax*twoax/a0;
        //std::cout<<"1::find cosine, variable c, solve(c^4 +("<<ce[0]<<")*c^3+("<<ce[1]<<")*c^2+("<<ce[2]<<")*c+("<<ce[3]<<")=0,c)\n";
        roots=RS_Math::quarticSolver(ce);
    } else {
        // Issue #1653: approximately a=b, solve the equation of ds^2/d\theta = 0 by Newton-Raphson
        double theta = ClosestEllipticPoint{a, b, ret}.getTheta();
        roots.push_back(std::cos(theta));
        // Just in case, the found solution is for the maximum distance. Then, the minimum is at the opposite
        roots.push_back(-roots.front());
    }
    if(roots.empty()) {
        //this should not happen
        std::cout<<"(a= "<<a<<" b= "<<b<<" x= "<<x<<" y= "<<y<<" )\n";
        std::cout<<"finding minimum for ("<<x<<"-"<<a<<"*cos(t))^2+("<<y<<"-"<<b<<"*sin(t))^2\n";
        std::cout<<"2::find cosine, variable c, solve(c^4 +("<<ce[0]<<")*c^3+("<<ce[1]<<")*c^2+("<<ce[2]<<")*c+("<<ce[3]<<")=0,c)\n";
        std::cout<<ce[0]<<' '<<ce[1]<<' '<<ce[2]<<' '<<ce[3]<<std::endl;
        std::cerr<<"RS_Math::RS_Ellipse::getNearestPointOnEntity() finds no root from quartic, this should not happen\n";
        return RS_Vector(coord); // better not to return invalid: return RS_Vector(false);
    }

//    RS_Vector vp2(false);
    double dDistance = RS_MAXDOUBLE*RS_MAXDOUBLE;
    //double ea;
    for(double cosTheta: roots) {
        //I don't understand the reason yet, but I can do without checking whether sine/cosine are valid
        //if ( std::abs(roots[i])>1.) continue;
        double const sinTheta=twoby*cosTheta/(twoax-twoa2b2*cosTheta); //sine
        //if (std::abs(s) > 1. ) continue;
        double const d2=twoa2b2+(twoax-2.*cosTheta*twoa2b2)*cosTheta+twoby*sinTheta;
        if (d2<0)
            continue; // fartherest
        RS_Vector vp3{a*cosTheta, b*sinTheta};
        double d=(vp3-ret).squared();
//        std::cout<<i<<" Checking: cos= "<<roots[i]<<" sin= "<<s<<" angle= "<<atan2(roots[i],s)<<" ds2= "<<d<<" d="<<d2<<std::endl;
        if( ret.valid && d>dDistance)
            continue;
        ret=vp3;
        dDistance=d;
//			ea=atan2(roots[i],s);
    }
    if( ! ret.valid ) {
        //this should not happen
//        std::cout<<ce[0]<<' '<<ce[1]<<' '<<ce[2]<<' '<<ce[3]<<std::endl;
//        std::cout<<"(x,y)=( "<<x<<" , "<<y<<" ) a= "<<a<<" b= "<<b<<" sine= "<<s<<" d2= "<<d2<<" dist= "<<d<<std::endl;
//        std::cout<<"RS_Ellipse::getNearestPointOnEntity() finds no minimum, this should not happen\n";
        RS_DEBUG->print(RS_Debug::D_ERROR,"RS_Ellipse::getNearestPointOnEntity() finds no minimum, this should not happen\n");
    }
    if (dist != nullptr) {
        *dist = std::sqrt(dDistance);
    }
    ret.rotate(getAngle());
    ret.move(getCenter());
//    ret=vp2;
    if (onEntity) {
        if (!RS_Math::isAngleBetween(getEllipseAngle(ret), getAngle1(), getAngle2(), isReversed())) { // not on entity, use the nearest endpoint
            //std::cout<<"not on ellipse, ( "<<getAngle1()<<" "<<getEllipseAngle(ret)<<" "<<getAngle2()<<" ) reversed= "<<isReversed()<<"\n";
            ret=getNearestEndpoint(coord,dist);
        }
    }

//    if(! ret.valid) {
//        std::cout<<"RS_Ellipse::getNearestOnEntity() returns invalid by mistake. This should not happen!"<<std::endl;
//    }
    return ret;
}

/**
 * @param tolerance Tolerance.
 *
 * @retval true if the given point is on this entity.
 * @retval false otherwise
 */
bool RS_Ellipse::isPointOnEntity(const RS_Vector& coord,
                                 double tolerance) const {
    double t=std::abs(tolerance);
    double a=getMajorRadius();
    double b=a*getRatio();
    RS_Vector vp((coord - getCenter()).rotate(-getAngle()));
    if ( a<RS_TOLERANCE ) {
        //radius treated as zero
        if(std::abs(vp.x)<RS_TOLERANCE && std::abs(vp.y) < b) return true;
        return false;
    }
    if ( b<RS_TOLERANCE ) {
        //radius treated as zero
        if (std::abs(vp.y)<RS_TOLERANCE && std::abs(vp.x) < a) return true;
        return false;
    }
    vp.scale(RS_Vector(1./a,1./b));

    if (std::abs(vp.squared()-1.) > t) return false;
	return RS_Math::isAngleBetween(vp.angle(),getAngle1(),getAngle2(),isReversed());
}

RS_Vector RS_Ellipse::getNearestCenter(const RS_Vector& coord,
                                       double* dist) const{
    RS_Vector   vCenter = data.center;
    double      distCenter = coord.distanceTo(data.center);

    RS_VectorSolutions  vsFoci = getFoci();
    if( 2 == vsFoci.getNumber()) {
        RS_Vector const& vFocus1 = vsFoci.get(0);
        RS_Vector const& vFocus2 = vsFoci.get(1);

        double distFocus1 = coord.distanceTo(vFocus1);
        double distFocus2 = coord.distanceTo(vFocus2);

        /* if (distFocus1 < distCenter) is true
         * then (distFocus1 < distFocus2) must be true too
         * and vice versa
         * no need to check this */
        if( distFocus1 < distCenter) {
            vCenter = vFocus1;
            distCenter = distFocus1;
        }
        else if( distFocus2 < distCenter) {
            vCenter = vFocus2;
            distCenter = distFocus2;
        }
    }

    if (dist != nullptr) {
        *dist = distCenter;
    }
    return vCenter;
}

/**
//create Ellipse with axes in x-/y- directions from 4 points
*
*
*@author Dongxu Li
*/
bool	RS_Ellipse::createFrom4P(const RS_VectorSolutions& sol){
    if (sol.getNumber() != 4 ) return (false); //only do 4 points
    std::vector<std::vector<double> > mt;
    std::vector<double> dn;
    int mSize(4);
    mt.resize(mSize);
    for(int i=0;i<mSize;i++) {//form the linear equation, c0 x^2 + c1 x + c2 y^2 + c3 y = 1
        mt[i].resize(mSize+1);
        mt[i][0]=sol.get(i).x * sol.get(i).x;
        mt[i][1]=sol.get(i).x ;
        mt[i][2]=sol.get(i).y * sol.get(i).y;
        mt[i][3]=sol.get(i).y ;
        mt[i][4]=1.;
    }
    if ( ! RS_Math::linearSolver(mt,dn)) return false;
    double d(1.+0.25*(dn[1]*dn[1]/dn[0]+dn[3]*dn[3]/dn[2]));
    if(std::abs(dn[0])<RS_TOLERANCE15
       ||std::abs(dn[2])<RS_TOLERANCE15
       ||d/dn[0]<RS_TOLERANCE15
       ||d/dn[2]<RS_TOLERANCE15
        ) {
        //ellipse not defined
        return false;
    }
    data.center.set(-0.5*dn[1]/dn[0],-0.5*dn[3]/dn[2]); // center
    d=sqrt(d/dn[0]);
    data.majorP.set(d,0.);
    data.ratio=sqrt(dn[0]/dn[2]);
    data.angle1=0.;
    data.angle2=0.;
//    DEBUG_HEADER
//    std::cout<<"center="<<data.center;
//    std::cout<<"majorP="<<data.majorP;
//    std::cout<<"ratio="<<data.ratio;
//    std::cout<<"successful"<<std::endl;
    return true;

}

/**
//create Ellipse with center and 3 points
*
*
*@author Dongxu Li
*/
bool	RS_Ellipse::createFromCenter3Points(const RS_VectorSolutions& sol) {
    if(sol.getNumber()<3) return false; //need one center and 3 points on ellipse
    std::vector<std::vector<double> > mt;
    int mSize(sol.getNumber() -1);
    if( (sol.get(mSize) - sol.get(mSize-1)).squared() < RS_TOLERANCE15 ) {
        //remove the last point
        mSize--;
    }

    mt.resize(mSize);
    std::vector<double> dn(mSize);
    switch(mSize){
        case 2:
            for(int i=0;i<mSize;i++){//form the linear equation
                mt[i].resize(mSize+1);
                RS_Vector vp(sol.get(i+1)-sol.get(0)); //the first vector is center
                mt[i][0]=vp.x*vp.x;
                mt[i][1]=vp.y*vp.y;
                mt[i][2]=1.;
            }
            if ( ! RS_Math::linearSolver(mt,dn) ) return false;
            if( dn[0]<RS_TOLERANCE15 || dn[1]<RS_TOLERANCE15) return false;
            setMajorP(RS_Vector(1./sqrt(dn[0]),0.));
            setRatio(sqrt(dn[0]/dn[1]));
            setAngle1(0.);
            setAngle2(0.);
            setCenter(sol.get(0));
            return true;

        case 3:
            for(int i=0;i<mSize;i++){//form the linear equation
                mt[i].resize(mSize+1);
                RS_Vector vp(sol.get(i+1)-sol.get(0)); //the first vector is center
                mt[i][0]=vp.x*vp.x;
                mt[i][1]=vp.x*vp.y;
                mt[i][2]=vp.y*vp.y;
                mt[i][3]=1.;
            }
            if ( ! RS_Math::linearSolver(mt,dn) ) return false;
            setCenter(sol.get(0));
            return createFromQuadratic(dn);
        default:
            return false;
    }
    return false; // only for compiler warning
}

/** \brief create from quadratic form:
  * dn[0] x^2 + dn[1] xy + dn[2] y^2 =1
  * keep the ellipse center before calling this function
  *
  *@author: Dongxu Li
  */
bool RS_Ellipse::createFromQuadratic(const std::vector<double>& dn){
    RS_DEBUG->print("RS_Ellipse::createFromQuadratic() begin\n");
    if(dn.size()!=3) return false;
//	if(std::abs(dn[0]) <RS_TOLERANCE2 || std::abs(dn[2])<RS_TOLERANCE2) return false; //invalid quadratic form

//eigenvalues and eigenvectors of quadratic form
    // (dn[0] 0.5*dn[1])
// (0.5*dn[1] dn[2])
    double a=dn[0];
    const double c=dn[1];
    double b=dn[2];

//Eigen system
    const double d = a - b;
    const double s=hypot(d,c);
// { a>b, d>0
// eigenvalue: ( a+b - s)/2, eigenvector: ( -c, d + s)
// eigenvalue: ( a+b + s)/2, eigenvector: ( d + s, c)
// }
// { a<b, d<0
// eigenvalue: ( a+b - s)/2, eigenvector: ( s-d,-c)
// eigenvalue: ( a+b + s)/2, eigenvector: ( c, s-d)
// }

// eigenvalues are required to be positive for ellipses
    if(s >= a+b )
        return false;
    if(a>=b) {
        setMajorP(RS_Vector(atan2(d+s, -c))/sqrt(0.5*(a+b-s)));
    }else{
        setMajorP(RS_Vector(atan2(-c, s-d))/sqrt(0.5*(a+b-s)));
    }
    setRatio(sqrt((a+b-s)/(a+b+s)));

// start/end angle at 0. means a whole ellipse, instead of an elliptic arc
    setAngle1(0.);
    setAngle2(0.);

    RS_DEBUG->print("RS_Ellipse::createFromQuadratic(): successful\n");
    return true;
}

bool RS_Ellipse::createFromQuadratic(const LC_Quadratic& q){
    if (!q.isQuadratic()) return false;
    auto  const& mQ=q.getQuad();
    double const& a=mQ(0,0);
    double const& c=2.*mQ(0,1);
    double const& b=mQ(1,1);
    auto  const& mL=q.getLinear();
    double const& d=mL(0);
    double const& e=mL(1);
    double determinant=c*c-4.*a*b;
    if (determinant>= -DBL_EPSILON)
        return false;
// find center of quadratic
// 2 A x + C y = D
// C x   + 2 B y = E
// x = (2BD - EC)/( 4AB - C^2)
// y = (2AE - DC)/(4AB - C^2)
    const RS_Vector eCenter=RS_Vector(2.*b*d - e*c, 2.*a*e - d*c)/determinant;
//generate centered quadratic
    LC_Quadratic qCentered=q;
    qCentered.move(-eCenter);
    if(qCentered.constTerm()>= -DBL_EPSILON) return false;
    const auto& mq2=qCentered.getQuad();
    const double factor=-1./qCentered.constTerm();
//quadratic terms
    if(!createFromQuadratic({mq2(0,0)*factor, 2.*mq2(0,1)*factor, mq2(1,1)*factor})) return false;

//move back to center
    move(eCenter);
    return true;
}

/**
//create Ellipse inscribed in a quadrilateral
*
*algorithm: http://chrisjones.id.au/Ellipses/ellipse.html
*finding the tangential points and ellipse center
*
*@author Dongxu Li
*/
bool RS_Ellipse::createInscribeQuadrilateral(const std::vector<RS_Line*>& lines, std::vector<RS_Vector> &tangent){
    if (lines.size() != 4)
        return false; //only do 4 lines

    std::vector<std::unique_ptr<RS_Line> > quad(4);
    { //form quadrilateral from intersections
        RS_EntityContainer c0(nullptr, false);
        for(RS_Line*const p: lines){//copy the line pointers
            c0.addEntity(p);
        }
        RS_VectorSolutions const& s0=RS_Information::createQuadrilateral(c0);
        if(s0.size()!=4)
            return false;
        for(size_t i=0; i<4; ++i){
            quad[i].reset(new RS_Line{s0[i], s0[(i+1)%4]});
        }
    }

//center of original square projected, intersection of diagonal
    RS_Vector centerProjection;
    {
        std::vector<RS_Line> diagonal;
        diagonal.emplace_back(quad[0]->getStartpoint(), quad[1]->getEndpoint());
        diagonal.emplace_back(quad[1]->getStartpoint(), quad[2]->getEndpoint());
        RS_VectorSolutions const& sol=RS_Information::getIntersectionLineLine( & diagonal[0],& diagonal[1]);
        if(sol.getNumber()==0) {//this should not happen
//        RS_DEBUG->print(RS_Debug::D_WARNING, "RS_Ellipse::createInscribeQuadrilateral(): can not locate projection Center");
            RS_DEBUG->print("RS_Ellipse::createInscribeQuadrilateral(): can not locate projection Center");
            return false;
        }
        centerProjection=sol.get(0);
    }
//        std::cout<<"RS_Ellipse::createInscribe(): centerProjection="<<centerProjection<<std::endl;

//	std::vector<RS_Vector> tangent;//holds the tangential points on edges, in the order of edges: 1 3 2 0
    int parallel=0;
    int parallel_index=0;
    for(int i=0;i<=1;++i) {
        RS_VectorSolutions const& sol1=RS_Information::getIntersectionLineLine(quad[i].get(), quad[(i+2)%4].get());
        RS_Vector direction;
        if(sol1.getNumber()==0) {
            direction=quad[i]->getEndpoint()-quad[i]->getStartpoint();
            ++parallel;
            parallel_index=i;
        }else{
            direction=sol1.get(0)-centerProjection;
        }
//                std::cout<<"Direction: "<<direction<<std::endl;
        RS_Line l(centerProjection, centerProjection+direction);
        for(int k=1;k<=3;k+=2){
            RS_VectorSolutions sol2=RS_Information::getIntersectionLineLine(&l, quad[(i+k)%4].get());
            if(sol2.size()) tangent.push_back(sol2.get(0));
        }
    }

    if(tangent.size()<3) return false;

//find ellipse center by projection
    RS_Vector ellipseCenter;
    {
        RS_Line cl0(quad[1]->getEndpoint(),(tangent[0]+tangent[2])*0.5);
        RS_Line cl1(quad[2]->getEndpoint(),(tangent[1]+tangent[2])*0.5);
        RS_VectorSolutions const& sol=RS_Information::getIntersection(&cl0, &cl1,false);
        if(sol.getNumber()==0){
//this should not happen
//        RS_DEBUG->print(RS_Debug::D_WARNING, "RS_Ellipse::createInscribeQuadrilateral(): can not locate Ellipse Center");
            RS_DEBUG->print("RS_Ellipse::createInscribeQuadrilateral(): can not locate Ellipse Center");
            return false;
        }
        ellipseCenter=sol.get(0);
    }
//	qDebug()<<"parallel="<<parallel;
    if(parallel==1){
        RS_DEBUG->print("RS_Ellipse::createInscribeQuadrilateral(): trapezoid detected\n");
//trapezoid
        RS_Line* l0=quad[parallel_index].get();
        RS_Line* l1=quad[(parallel_index+2)%4].get();
        RS_Vector centerPoint=(l0->getMiddlePoint()+l1->getMiddlePoint())*0.5;
//not symmetric, no inscribed ellipse
        if( std::abs(centerPoint.distanceTo(l0->getStartpoint()) - centerPoint.distanceTo(l0->getEndpoint()))>RS_TOLERANCE)
            return false;
//symmetric
        RS_DEBUG->print("RS_Ellipse::createInscribeQuadrilateral(): symmetric trapezoid detected\n");
        double d=l0->getDistanceToPoint(centerPoint);
        double l=((l0->getLength()+l1->getLength()))*0.25;
        double k= 4.*d/std::abs(l0->getLength()-l1->getLength());
        double theta=d/(l*k);
        if(theta>=1. || d<RS_TOLERANCE) {
            RS_DEBUG->print("RS_Ellipse::createInscribeQuadrilateral(): this should not happen\n");
            return false;
        }
        theta=asin(theta);

//major axis
        double a=d/(k*tan(theta));
        setCenter(RS_Vector(0., 0.));
        setMajorP(RS_Vector(a, 0.));
        setRatio(d/a);
        rotate(l0->getAngle1());
        setCenter(centerPoint);
        return true;

    }
//    double ratio;
//        std::cout<<"dn="<<dn[0]<<' '<<dn[1]<<' '<<dn[2]<<std::endl;
    std::vector<double> dn(3);
    RS_Vector angleVector(false);

    for(size_t i=0;i<tangent.size();i++) {
        tangent[i] -= ellipseCenter;//relative to ellipse center
    }
    std::vector<std::vector<double> > mt;
    mt.clear();
    const double symTolerance=20.*RS_TOLERANCE;
    for(const RS_Vector& vp: tangent){
//form the linear equation
// need to remove duplicated {x^2, xy, y^2} terms due to symmetry (x => -x, y=> -y)
// i.e. rotation of 180 degrees around ellipse center
//		std::cout<<"point  : "<<vp<<std::endl;
        std::vector<double> mtRow;
        mtRow.push_back(vp.x*vp.x);
        mtRow.push_back(vp.x*vp.y);
        mtRow.push_back(vp.y*vp.y);
        const double l= hypot(hypot(mtRow[0], mtRow[1]), mtRow[2]);
        bool addRow(true);
        for(const auto& v: mt){
            RS_Vector const dv{v[0] - mtRow[0], v[1] - mtRow[1], v[2] - mtRow[2]};
            if( dv.magnitude() < symTolerance*l){
                //symmetric
                addRow=false;
                break;
            }
        }
        if(addRow) {
            mtRow.push_back(1.);
            mt.push_back(mtRow);
        }
    }
//    std::cout<<"mt.size()="<<mt.size()<<std::endl;
    switch(mt.size()){
        case 2:{// the quadrilateral is a parallelogram
            RS_DEBUG->print("RS_Ellipse::createInscribeQuadrilateral(): parallelogram detected\n");

//fixme, need to handle degenerate case better
//        double angle(center.angleTo(tangent[0]));
            RS_Vector majorP(tangent[0]);
            double dx(majorP.magnitude());
            if(dx<RS_TOLERANCE2) return false; //refuse to return zero size ellipse
            angleVector.set(majorP.x/dx,-majorP.y/dx);
            for(size_t i=0;i<tangent.size();i++)tangent[i].rotate(angleVector);

            RS_Vector minorP(tangent[2]);
            double dy2(minorP.squared());
            if(std::abs(minorP.y)<RS_TOLERANCE || dy2<RS_TOLERANCE2) return false; //refuse to return zero size ellipse
// y'= y
// x'= x-y/tan
// reverse scale
// y=y'
// x=x'+y' tan
//
            double ia2=1./(dx*dx);
            double ib2=1./(minorP.y*minorP.y);
//ellipse scaled:drawi
// ia2*x'^2+ib2*y'^2=1
// ia2*(x-y*minor.x/minor.y)^2+ib2*y^2=1
// ia2*x^2 -2*ia2*minor.x/minor.y xy + ia2*minor.x^2*ib2 y^2 + ib2*y^2 =1
            dn[0]=ia2;
            dn[1]=-2.*ia2*minorP.x/minorP.y;
            dn[2]=ib2*ia2*minorP.x*minorP.x+ib2;
        }
            break;
        case 4:
            mt.pop_back(); //only 3 points needed to form the qudratic form
            if ( ! RS_Math::linearSolver(mt,dn) ) return false;
            break;
        default:
            RS_DEBUG->print(RS_Debug::D_WARNING,"No inscribed ellipse for non isosceles trapezoid");
            return false; //invalid quadrilateral
    }

    if(! createFromQuadratic(dn)) return false;
    setCenter(ellipseCenter);

    if(angleVector.valid) {//need to rotate back, for the parallelogram case
        angleVector.y *= -1.;
        rotate(ellipseCenter,angleVector);
    }
    return true;

}

/**
 * a naive implementation of middle point
 * to accurately locate the middle point from arc length is possible by using elliptic integral to find the total arc length, then, using elliptic function to find the half length point
 */
RS_Vector RS_Ellipse::getMiddlePoint()const{
    return getNearestMiddle(getCenter());
}
/**
  * get Nearest equidistant point
  *
  *@author: Dongxu Li
  */
RS_Vector RS_Ellipse::getNearestMiddle(const RS_Vector& coord,
                                       double* dist,
                                       int middlePoints
) const{
    RS_DEBUG->print("RS_Ellpse::getNearestMiddle(): begin\n");
    if ( ! isEllipticArc() ) {
        //no middle point for whole ellipse, angle1=angle2=0
        if (dist) {
            *dist = RS_MAXDOUBLE;
        }
        return RS_Vector(false);
    }
    double ra(getMajorRadius());
    double rb(getRatio()*ra);
    if ( ra < RS_TOLERANCE || rb < RS_TOLERANCE ) {
        //zero radius, return the center
        RS_Vector vp(getCenter());
        if (dist) {
            *dist = vp.distanceTo(coord);
        }
        return vp;
    }
    double amin=getCenter().angleTo(getStartpoint());
    double amax=getCenter().angleTo(getEndpoint());
    if(isReversed()) {
        std::swap(amin,amax);
    }
    double da=std::fmod(amax-amin+2.*M_PI, 2.*M_PI);
    if ( da < RS_TOLERANCE ) {
        da = 2.*M_PI; //whole ellipse
    }
    RS_Vector vp(getNearestPointOnEntity(coord,true,dist));
    double a=getCenter().angleTo(vp);
    int counts(middlePoints + 1);
    int i = static_cast<int>(std::fmod(a-amin+2.*M_PI,2.*M_PI)/da*counts+0.5);
    // remove end points
    i = std::max(i, 1);
    i = std::min(i, counts - 1);
    a=amin + da*(double(i)/double(counts))-getAngle();
    vp.set(a);
    RS_Vector vp2(vp);
    vp2.scale( RS_Vector(1./ra,1./rb));
    vp.scale(1./vp2.magnitude());
    vp.rotate(getAngle());
    vp.move(getCenter());

    if (dist != nullptr) {
        *dist = vp.distanceTo(coord);
    }
    //RS_DEBUG->print("RS_Ellipse::getNearestMiddle: angle1=%g, angle2=%g, middle=%g\n",amin,amax,a);
    RS_DEBUG->print("RS_Ellpse::getNearestMiddle(): end\n");
    return vp;
}

/**
  * get the tangential point of a tangential line orthogonal to a given line
  *@ normal, the given line
  *@ onEntity, should the tangential be required to on entity of the elliptic arc
  *@ coord, current cursor position
  *
  *@author: Dongxu Li
  */

RS_Vector RS_Ellipse::getNearestOrthTan(
    const RS_Vector &coord,
    const RS_Line &normal,
    bool onEntity) const{
    if (!coord.valid){
        return RS_Vector(false);
    }
    RS_Vector direction = normal.getEndpoint() - normal.getStartpoint();
    if (direction.squared() < RS_TOLERANCE15){
        //undefined direction
        return RS_Vector(false);
    }
    //scale to ellipse angle
    RS_Vector aV(-getAngle());
    direction.rotate(aV);
    double angle = direction.scale(RS_Vector(1., getRatio())).angle();
    double ra(getMajorRadius());
    direction.set(ra * cos(angle), getRatio() * ra * sin(angle));//relative to center
    std::vector<RS_Vector> sol;
    for (int i = 0; i < 2; i++) {
        if (!onEntity ||
            RS_Math::isAngleBetween(angle, getAngle1(), getAngle2(), isReversed())){
            sol.push_back(i == 0 ? direction : - direction);
        }
        angle = RS_Math::correctAngle(angle + M_PI);
    }
    if (sol.size() < 1)
        return RS_Vector(false);
    aV.y *= -1.;
    for (auto &v: sol) {
        v.rotate(aV);
    }
    RS_Vector vp{};
    switch (sol.size()) {
        case 0:
            return RS_Vector(false);
        case 2:
            if (RS_Vector::dotP(sol[1], coord - getCenter()) > 0.){
                vp = sol[1];
                break;
            }
            // fall-through
        default:
            vp = sol[0];
            break;
    }
    return getCenter() + vp;
}

double RS_Ellipse::getBulge() const
{
    double bulge = std::tan(std::abs(getAngleLength()) / 4.0);
    return isReversed() ? -bulge : bulge;
}

RS_Vector RS_Ellipse::dualLineTangentPoint(const RS_Vector& line) const{
    // u x + v y = 1
    // coordinates : dual
    // rotate (-a) : rotate(a)
    RS_Vector uv = RS_Vector{line}.rotate(-data.majorP.angle());
    // slope = -b c/ a s ( a s, - b c)
    // x a s - b c y =0 -> s/c = b y / a x
    // elliptical angle
    double t = std::atan2(data.ratio * uv.y, uv.x);
    RS_Vector vp{data.majorP.magnitude()*std::cos(t), data.majorP.magnitude()*data.ratio*std::sin(t)};
    vp.rotate(data.majorP.angle());

    RS_Vector vp0 = data.center + vp;
    RS_Vector vp1 = data.center - vp;
    auto lineEqu = [&line](const RS_Vector& vp) {
        return std::abs(line.dotP(vp) + 1.);
    };
    return lineEqu(vp0) < lineEqu(vp1) ? vp0 : vp1;
}

void RS_Ellipse::move(const RS_Vector& offset) {
    data.center.move(offset);
    //calculateEndpoints();
    //    minV.move(offset);
    //    maxV.move(offset);
    moveBorders(offset);
}

void RS_Ellipse::rotate(const RS_Vector& center, double angle) {
    RS_Vector angleVector(angle);
    data.center.rotate(center, angleVector);
    data.majorP.rotate(angleVector);
    //calculateEndpoints();
    calculateBorders();
}

void RS_Ellipse::revertDirection(){
    if (data.isArc) {
        std::swap(data.angle1, data.angle2);
        data.reversed = !data.reversed;
        calculateBorders();
    }
}

void RS_Ellipse::rotate(const RS_Vector& center, const RS_Vector& angleVector) {
    data.center.rotate(center, angleVector);
    data.majorP.rotate(angleVector);
    //calculateEndpoints();
    calculateBorders();
}

void RS_Ellipse::rotate(double angle) {//rotate around origin
    RS_Vector aV(angle);
    data.center.rotate(aV);
    data.majorP.rotate(aV);
    calculateBorders();
}

void RS_Ellipse::rotate(const RS_Vector& angleVector) {//rotate around origin
    data.center.rotate(angleVector);
    data.majorP.rotate(angleVector);
    //calculateEndpoints();
    calculateBorders();
}

/**
 * make sure angleLength() is not more than 2*M_PI
 */
void RS_Ellipse::correctAngles() {
    double *pa1= & data.angle1;
    double *pa2= & data.angle2;
    if (isReversed()) std::swap(pa1,pa2);
    *pa2 = *pa1 + std::fmod(*pa2 - *pa1, 2.*M_PI);
    if ( std::abs(data.angle1 - data.angle2) < RS_TOLERANCE_ANGLE && (std::abs(data.angle1) > RS_TOLERANCE_ANGLE)) {
        // we need this only if there are actual angles (arc). otherwise, adding 2pi will transform ellipse to
        // elliptic arc
        *pa2 += 2.*M_PI;
    }
}

void RS_Ellipse::moveStartpoint(const RS_Vector& pos) {
    data.angle1 = getEllipseAngle(pos);
    //data.angle1 = data.center.angleTo(pos);
    //calculateEndpoints();
    correctAngles(); // make sure angleLength is no more than 2*M_PI
    calculateBorders();
}

void RS_Ellipse::moveEndpoint(const RS_Vector& pos) {
    data.angle2 = getEllipseAngle(pos);
    //data.angle2 = data.center.angleTo(pos);
    //calculateEndpoints();
    correctAngles(); // make sure angleLength is no more than 2*M_PI
    calculateBorders();
}


RS2::Ending RS_Ellipse::getTrimPoint(const RS_Vector& trimCoord,
                                     const RS_Vector& /*trimPoint*/) {

    //double angEl = getEllipseAngle(trimPoint);
    double angM = getEllipseAngle(trimCoord);
    if (RS_Math::getAngleDifference(angM, data.angle1,isReversed()) > RS_Math::getAngleDifference(data.angle2,angM,isReversed())) {
        return RS2::EndingStart;
    } else {
        return RS2::EndingEnd;
    }
}

RS_Vector RS_Ellipse::prepareTrim(const RS_Vector& trimCoord,
                                  const RS_VectorSolutions& trimSol) {
//special trimming for ellipse arc
        RS_DEBUG->print("RS_Ellipse::prepareTrim()");
    if(!trimSol.hasValid())
            return RS_Vector{false};
    if(trimSol.getNumber() == 1)
        return trimSol.front();
    double am=getEllipseAngle(trimCoord);
	std::vector<double> ias;
    double ia(0.),ia2(0.);
    RS_Vector is,is2;
	for(size_t ii=0; ii<trimSol.getNumber(); ++ii) { //find closest according ellipse angle
		ias.push_back(getEllipseAngle(trimSol.get(ii)));
        if( !ii ||  std::abs( remainder( ias[ii] - am, 2*M_PI)) < std::abs( remainder( ia -am, 2*M_PI)) ) {
            ia = ias[ii];
            is = trimSol.get(ii);
        }
    }
    std::sort(ias.begin(),ias.end());
	for(size_t ii=0; ii<trimSol.getNumber(); ++ii) { //find segment to include trimCoord
        if ( ! RS_Math::isSameDirection(ia,ias[ii],RS_TOLERANCE)) continue;
        if( RS_Math::isAngleBetween(am,ias[(ii+trimSol.getNumber()-1)% trimSol.getNumber()],ia,false))  {
            ia2=ias[(ii+trimSol.getNumber()-1)% trimSol.getNumber()];
        } else {
            ia2=ias[(ii+1)% trimSol.getNumber()];
        }
        break;
    }
	for(const RS_Vector& vp: trimSol) { //find segment to include trimCoord
		if ( ! RS_Math::isSameDirection(ia2,getEllipseAngle(vp),RS_TOLERANCE)) continue;
		is2=vp;
        break;
    }
    if(RS_Math::isSameDirection(getAngle1(),getAngle2(),RS_TOLERANCE_ANGLE)
            ||  RS_Math::isSameDirection(ia2,ia,RS_TOLERANCE) ) {
        //whole ellipse
        if( !RS_Math::isAngleBetween(am,ia,ia2,isReversed())) {
            std::swap(ia,ia2);
            std::swap(is,is2);
        }
        setAngle1(ia);
        setAngle2(ia2);
        double da1=std::abs(remainder(getAngle1()-am,2*M_PI));
        double da2=std::abs(remainder(getAngle2()-am,2*M_PI));
        if(da2<da1) {
            std::swap(is,is2);
        }

    } else {
        double dia=std::abs(remainder(ia-am,2*M_PI));
        double dia2=std::abs(remainder(ia2-am,2*M_PI));
        double ai_min=std::min(dia,dia2);
        double da1=std::abs(remainder(getAngle1()-am,2*M_PI));
        double da2=std::abs(remainder(getAngle2()-am,2*M_PI));
        double da_min=std::min(da1,da2);
        if( da_min < ai_min ) {
            //trimming one end of arc
            bool irev= RS_Math::isAngleBetween(am,ia2,ia, isReversed()) ;
            if ( RS_Math::isAngleBetween(ia,getAngle1(),getAngle2(), isReversed()) &&
                    RS_Math::isAngleBetween(ia2,getAngle1(),getAngle2(), isReversed()) ) { //
                if(irev) {
                    setAngle2(ia);
                    setAngle1(ia2);
                } else {
                    setAngle1(ia);
                    setAngle2(ia2);
                }
                da1=std::abs(remainder(getAngle1()-am,2*M_PI));
                da2=std::abs(remainder(getAngle2()-am,2*M_PI));
            }
            if( ((da1 < da2) && (RS_Math::isAngleBetween(ia2,ia,getAngle1(),isReversed()))) ||
                    ((da1 > da2) && (RS_Math::isAngleBetween(ia2,getAngle2(),ia,isReversed())))
              ) {
                std::swap(is,is2);
                //std::cout<<"reset: angle1="<<getAngle1()<<" angle2="<<getAngle2()<<" am="<< am<<" is="<<getEllipseAngle(is)<<" ia2="<<ia2<<std::endl;
            }
        } else {
            //choose intersection as new end
            if( dia > dia2) {
                std::swap(is,is2);
                std::swap(ia,ia2);
            }
            if(RS_Math::isAngleBetween(ia,getAngle1(),getAngle2(),isReversed())) {
                if(std::abs(ia - getAngle1()) > RS_TOLERANCE_ANGLE && RS_Math::isAngleBetween(am,getAngle1(),ia,isReversed())) {
                    setAngle2(ia);
                } else {
                    setAngle1(ia);
                }
            }
        }
    }
    return is;
}

double RS_Ellipse::getEllipseAngle(const RS_Vector& pos) const {
    RS_Vector m = pos-data.center;
    m.rotate(-data.majorP.angle());
    m.x *= data.ratio;
    return m.angle();
}

const RS_EllipseData& RS_Ellipse::getData() const{
	return data;
}

/* Dongxu Li's Version, 19 Aug 2011
 * scale an ellipse
 * Find the eigen vectors and eigen values by optimization
 * original ellipse equation,
 * x= a cos t
 * y= b sin t
 * rotated by angle,
 *
 * x = a cos t cos (angle) - b sin t sin(angle)
 * y = a cos t sin (angle) + b sin t cos(angle)
 * scaled by ( kx, ky),
 * x *= kx
 * y *= ky
 * find the maximum and minimum of x^2 + y^2,
 */
void RS_Ellipse::scale(const RS_Vector& center, const RS_Vector& factor) {
    RS_Vector vpStart;
    RS_Vector vpEnd;
    if(isEllipticArc()){
        //only handle start/end points for ellipse arc
        vpStart=getStartpoint().scale(center,factor);
        vpEnd=getEndpoint().scale(center,factor);
    }
    data.center.scale(center, factor);
    RS_Vector vp1(getMajorP());
    double a(vp1.magnitude());
    if(a<RS_TOLERANCE) return; //ellipse too small
    vp1 *= 1./a;
    double ct=vp1.x;
    double ct2 = ct*ct; // cos^2 angle
    double st=vp1.y;
    double st2=1.0 - ct2; // sin^2 angle
    double kx2= factor.x * factor.x;
    double ky2= factor.y * factor.y;
//    double a=getMajorRadius();
    double b=getRatio()*a;
    double cA=0.5*a*a*(kx2*ct2+ky2*st2);
    double cB=0.5*b*b*(kx2*st2+ky2*ct2);
    double cC=a*b*ct*st*(ky2-kx2);
    if (factor.x < 0)
        setReversed(!isReversed());
    if (factor.y < 0)
        setReversed(!isReversed());
    RS_Vector vp(cA-cB,cC);
    vp1.set(a,b);
    vp1.scale(RS_Vector(0.5*vp.angle()));
    vp1.rotate(RS_Vector(ct,st));
    vp1.scale(factor);
    setMajorP(vp1);
    a=cA+cB;
    b=vp.magnitude();
    setRatio( sqrt((a - b)/(a + b) ));
    if( isEllipticArc() ) {
        //only reset start/end points for ellipse arcs, i.e., angle1 angle2 are not both zero
        setAngle1(getEllipseAngle(vpStart));
        setAngle2(getEllipseAngle(vpEnd));
        correctAngles();//avoid extra 2.*M_PI in angles
    }

    //calculateEndpoints();
    scaleBorders(center,factor);
// calculateBorders();

}

/**
 * @author{Dongxu Li}
 */
RS_Entity& RS_Ellipse::shear(double k)
{
    RS_Ellipse e1 = *this;
    e1.createFromQuadratic(e1.getQuadratic().shear(k));
    if (isArc()) {
        e1.moveStartpoint(getStartpoint().shear(k));
        e1.moveEndpoint(getEndpoint().shear(k));
    }
    *this = e1;
    return *this;
}

/**
 * is the Ellipse an Arc
 * @return false, if both angle1/angle2 are zero
 *
 *@author: Dongxu Li
 */
bool RS_Ellipse::isEllipticArc() const{
/*#ifndef EMU_C99
    using std::isnormal;
#endif
    return *//*std::*//*isnormal(getAngle1()) || *//*std::*//*isnormal(getAngle2());*/
   return data.isArc;
}

/**
 * mirror by the axis of the line axisPoint1 and axisPoint2
 *
 *@author: Dongxu Li
 */
void RS_Ellipse::mirror(const RS_Vector& axisPoint1, const RS_Vector& axisPoint2) {
    RS_Vector center=getCenter();
    RS_Vector majorp = center + getMajorP();
    RS_Vector startpoint,endpoint;
    bool isArc = isEllipticArc();
    if (isArc)  {
        startpoint = getStartpoint();
        endpoint = getEndpoint();
    }

    center.mirror(axisPoint1, axisPoint2);
    majorp.mirror(axisPoint1, axisPoint2);

    setCenter(center);
    setReversed(!isReversed());
    setMajorP(majorp - center);
    if( isArc )  {
        //only reset start/end points for ellipse arcs, i.e., angle1 angle2 are not both zero
        startpoint.mirror(axisPoint1, axisPoint2);
        endpoint.mirror(axisPoint1, axisPoint2);
        setAngle1( getEllipseAngle(startpoint));
        setAngle2( getEllipseAngle(endpoint));
        correctAngles();//avoid extra 2.*M_PI in angles
    }
    calculateBorders();
}

/**
  * get direction1 and direction2
  * get the tangent pointing outside at end points
  *
  *@author: Dongxu Li
  */
//getDirection1 for start point
double RS_Ellipse::getDirection1() const {
    RS_Vector vp;
    if (isReversed()){
        vp.set(sin(getAngle1()), -getRatio()*cos(getAngle1()));
    } else {
        vp.set(-sin(getAngle1()), getRatio()*cos(getAngle1()));
    }
    return vp.angle()+getAngle();
}

//getDirection2 for end point
double RS_Ellipse::getDirection2() const {
    RS_Vector vp;
    if (isReversed()){
        vp.set(-sin(getAngle2()), getRatio()*cos(getAngle2()));
    } else {
        vp.set(sin(getAngle2()), -getRatio()*cos(getAngle2()));
    }
    return vp.angle()+getAngle();
}

void RS_Ellipse::moveRef(const RS_Vector& ref, const RS_Vector& offset) {
    if(isEllipticArc()){
        RS_Vector startpoint = getStartpoint();
        RS_Vector endpoint = getEndpoint();

        //    if (ref.distanceTo(startpoint)<1.0e-4) {
//instead of
        if ((ref-startpoint).squared()<RS_TOLERANCE_ANGLE) {
            moveStartpoint(startpoint+offset);
            correctAngles();//avoid extra 2.*M_PI in angles // todo - is this call really necessary? It is called in moveStartpoint() already
            return;
        }
        if ((ref-endpoint).squared()<RS_TOLERANCE_ANGLE) {
            moveEndpoint(endpoint+offset);
//            correctAngles();//avoid extra 2.*M_PI in angles // todo - is this call really necessary? It is called in moveStartpoint() already
            return;
        }
    }
    if ((ref-getCenter()).squared()<RS_TOLERANCE_ANGLE) {
        //move center
        setCenter(getCenter()+offset);
        calculateBorders();
        return;
    }

    if(data.ratio>1.) {
        switchMajorMinor();
    }
    auto foci=getFoci();
    for(size_t i=0; i< 2 ; i++){
        if ((ref-foci.at(i)).squared()<RS_TOLERANCE_ANGLE) {
            auto focusNew=foci.at(i) + offset;
            //move focus
            auto center = getCenter() + offset*0.5;
            RS_Vector majorP;
            if(getMajorP().dotP( foci.at(i) - getCenter()) >= 0.){
                majorP = focusNew - center;
            }else{
                majorP = center - focusNew;
            }
            double d=getMajorP().magnitude();
            double c=0.5*focusNew.distanceTo(foci.at(1-i));
            double k=majorP.magnitude();
            if(k<RS_TOLERANCE2 || d < RS_TOLERANCE ||
               c >= d - RS_TOLERANCE) return;
            //            DEBUG_HEADER
            //            std::cout<<__func__<<" : moving focus";
            majorP *= d/k;
            setCenter(center);
            setMajorP(majorP);
            setRatio(sqrt(d*d-c*c)/d);
            correctAngles();//avoid extra 2.*M_PI in angles
            if(data.ratio>1.) {
                switchMajorMinor();
            }
            else{
                calculateBorders();
            }
            return;
        }
    }

    //move major/minor points
    if ((ref-getMajorPoint()).squared()<RS_TOLERANCE_ANGLE) {
        RS_Vector majorP=getMajorP()+offset;
        double r=majorP.magnitude();
        if(r<RS_TOLERANCE) return;
        double ratio = getRatio()*getMajorRadius()/r;
        setMajorP(majorP);
        setRatio(ratio);
        if(data.ratio>1.) {
            switchMajorMinor();
        }
        else{
            calculateBorders();
        }
        return;
    }
    if ((ref-getMinorPoint()).squared()<RS_TOLERANCE_ANGLE) {
        RS_Vector minorP=getMinorPoint() + offset;
        double r2=getMajorP().squared();
        if(r2<RS_TOLERANCE2) return;
        RS_Vector projected= getCenter() +
                             getMajorP()*getMajorP().dotP(minorP-getCenter())/r2;
        double r=(minorP - projected).magnitude();
        if(r<RS_TOLERANCE) return;
        double ratio = getRatio()*r/getMinorRadius();
        setRatio(ratio);
        if(data.ratio>1.) {
            switchMajorMinor();
        }
        else{
            calculateBorders();
        }
        return;
    }
}

/** return the equation of the entity
for quadratic,

return a vector contains:
m0 x^2 + m1 xy + m2 y^2 + m3 x + m4 y + m5 =0

for linear:
m0 x + m1 y + m2 =0
**/
LC_Quadratic RS_Ellipse::getQuadratic() const
{
    std::vector<double> ce(6,0.);
    ce[0]=data.majorP.squared();
    ce[2]= data.ratio*data.ratio*ce[0];
    if(ce[0]<RS_TOLERANCE2 || ce[2]<RS_TOLERANCE2){
        return LC_Quadratic();
    }
    ce[0]=1./ce[0];
    ce[2]=1./ce[2];
    ce[5]=-1.;
    LC_Quadratic ret(ce);
    ret.rotate(getAngle());
    ret.move(data.center);
    return ret;
}

/**
 * @brief areaLineIntegral, line integral for contour area calculation by Green's Theorem
 * Contour Area =\oint x dy
 * @return line integral \oint x dy along the entity
 * \oint x dy = Cx y + \frac{1}{4}((a^{2}+b^{2})sin(2a)cos^{2}(t)-ab(2sin^{2}(a)sin(2t)-2t-sin(2t)))
 *@author Dongxu Li
 */
double RS_Ellipse::areaLineIntegral() const{
    const double a=getMajorRadius();
    const double b=getMinorRadius();
    if(!isEllipticArc())
        return M_PI*a*b;
    const double ab=a*b;
    const double r2=a*a+b*b;
    const double& cx=data.center.x;
    const double aE=getAngle();
    const double& a0=data.angle1;
    const double& a1=data.angle2;
    const double fStart=cx*getStartpoint().y+0.25*r2*sin(2.*aE)*cos(a0)*cos(a0)-0.25*ab*(2.*sin(aE)*sin(aE)*sin(2.*a0)-sin(2.*a0));
    const double fEnd=cx*getEndpoint().y+0.25*r2*sin(2.*aE)*cos(a1)*cos(a1)-0.25*ab*(2.*sin(aE)*sin(aE)*sin(2.*a1)-sin(2.*a1));
    if (isReversed()) {
        return fEnd-fStart - 0.5 * a * b * getAngleLength();
    } else {
        return fEnd-fStart + 0.5 * a * b * getAngleLength();
    }
}

bool RS_Ellipse::isReversed() const {
	return data.reversed;
}

void RS_Ellipse::setReversed(bool r) {
	data.reversed = r;
}

double RS_Ellipse::getAngle() const {
	return data.majorP.angle();
}

double RS_Ellipse::getAngle1() const {
	return data.angle1;
}

void RS_Ellipse::setAngle1(double a1) {
	data.angle1 = a1;
}

double RS_Ellipse::getAngle2() const {
	return data.angle2;
}

void RS_Ellipse::setAngle2(double a2) {
	data.angle2 = a2;
}

RS_Vector RS_Ellipse::getCenter() const {
	return data.center;
}

void RS_Ellipse::setCenter(const RS_Vector& c) {
	data.center = c;
}


const RS_Vector& RS_Ellipse::getMajorP() const {
	return data.majorP;
}

void RS_Ellipse::setMajorP(const RS_Vector& p) {
	data.majorP = p;
}

double RS_Ellipse::getRatio() const {
	return data.ratio;
}

void RS_Ellipse::setRatio(double r) {
	data.ratio = r;
}

double RS_Ellipse::getAngleLength() const {
    double a = getAngle1();
    double b = getAngle2();

    if (isReversed())
        std::swap(a, b);
    double ret = RS_Math::correctAngle(b - a);
    // full ellipse:
    if (std::abs(std::remainder(ret, 2. * M_PI)) < RS_TOLERANCE_ANGLE) {
        ret = 2 * M_PI;
    }

    return ret;
}


double RS_Ellipse::getMajorRadius() const {
	return data.majorP.magnitude(); // fixme - renderperf - cache !!!!!
}

RS_Vector RS_Ellipse::getMajorPoint() const{
	return data.center + data.majorP;
}

RS_Vector RS_Ellipse::getMinorPoint() const{
	return data.center +
			RS_Vector(-data.majorP.y, data.majorP.x)*data.ratio;
}

double RS_Ellipse::getMinorRadius() const {
	return data.majorP.magnitude()*data.ratio;
}

void RS_Ellipse::draw(RS_Painter* painter) {
    // Adjust dash offset
    painter->updateDashOffset(this);
    if (data.isArc){
        painter->drawEllipseArcWCS(data.center, getMajorRadius(), data.ratio, data.angleDegrees,
                                data.startAngleDegrees, data.otherAngleDegrees,
                                data.angularLength, data.reversed);
    }
    else {
        painter->drawEllipseWCS(data.center, getMajorRadius(), data.ratio, data.angleDegrees);
    }
}

/**
 * Dumps the point's data to stdout.
 */
std::ostream& operator << (std::ostream& os, const RS_Ellipse& a) {
    os << " Ellipse: " << a.data << "\n";
    return os;
}
