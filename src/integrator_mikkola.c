/**
 * @file 	integrator.c
 * @brief 	Mikkola integration scheme.
 * @author 	Hanno Rein <hanno@hanno-rein.de>
 * @detail	This file implements the Mikkola integration scheme.  
 * 
 * @section 	LICENSE
 * Copyright (c) 2015 Hanno Rein
 *
 * This file is part of rebound.
 *
 * rebound is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * rebound is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with rebound.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "particle.h"
#include "main.h"
#include "tools.h"
#include "gravity.h"
#include "boundaries.h"

#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define MIN(a, b) ((a) > (b) ? (b) : (a))

// Slightly dirty trick to rename function for librebound use
#ifdef LIBREBOUND
#define integrator_part1                      integrator_mikkola_part1
#define integrator_part2                      integrator_mikkola_part2
#define integrator_force_is_velocitydependent integrator_mikkola_force_is_velocitydependent
#define integrator_epsilon                    integrator_mikkola_epsilon
#define integrator_min_dt                     integrator_mikkola_min_dt
#define integrator_reset                      integrator_mikkola_reset
#endif // LIBREBOUND

// These variables have no effect for constant timestep integrators.
int integrator_force_is_velocitydependent 	= 1;
double integrator_epsilon 			= 0;
double integrator_min_dt 			= 0;

static struct particle* restrict p_j  = NULL;
static double* restrict eta = NULL;
static double Mtotali;
static unsigned int integrator_timestep_warning = 0;

// Fast inverse factorial lookup table
static const double invfactorial[35] = {1., 1., 1./2., 1./6., 1./24., 1./120., 1./720., 1./5040., 1./40320., 1./362880., 1./3628800., 1./39916800., 1./479001600., 1./6227020800., 1./87178291200., 1./1307674368000., 1./20922789888000., 1./355687428096000., 1./6402373705728000., 1./121645100408832000., 1./2432902008176640000., 1./51090942171709440000., 1./1124000727777607680000., 1./25852016738884976640000., 1./620448401733239439360000., 1./15511210043330985984000000., 1./403291461126605635584000000., 1./10888869450418352160768000000., 1./304888344611713860501504000000., 1./8841761993739701954543616000000., 1./265252859812191058636308480000000., 1./8222838654177922817725562880000000., 1./263130836933693530167218012160000000., 1./8683317618811886495518194401280000000., 1./295232799039604140847618609643520000000.};

//static double ipow(double base, unsigned int exp) {
//	double result = 1;
//	while (exp) {
//		if (exp & 1)
//		    result *= base;
//		exp >>= 1;
//		base *= base;
//	}
//	return result;
//}

static inline double fastabs(double x){
	    return (x > 0.) ? x : -x;
}

static void stumpff_cs(double *restrict cs, double z) {
	unsigned int n = 0;
	while(z>0.1){
		z = z/4.;
		n++;
	}
	double zm = -z;
	cs[4] = invfactorial[4] - z*invfactorial[6]; 	// always calculate first two terms
	cs[5] = invfactorial[5] - z*invfactorial[7]; 	// always calculate first two terms
	double old_c_4;
	double _pow = zm;
	unsigned int k=8;
	do{
		old_c_4 = cs[4];
		_pow *= zm;
		cs[4] += _pow*invfactorial[k];
		k+=1;
		cs[5] += _pow*invfactorial[k];
		k+=1;
	}while(cs[4]!=old_c_4 && k<35);			// Stop if new term smaller than machine precision (cs[5] converges faster than cs[4])
	cs[3] = 1./6.-z*cs[5];
	cs[2] = 0.5-z*cs[4];
	cs[1] = 1.-z*cs[3];
	for (;n>0;n--){	
		z = z*4.;
		cs[5] = (cs[5]+cs[4]+cs[3]*cs[2])*0.0625;
		cs[4] = (1.+cs[1])*cs[3]*0.125;
		cs[3] = 1./6.-z*cs[5];
		cs[2] = 0.5-z*cs[4];
		cs[1] = 1.-z*cs[3];
	}
	cs[0] = 1.-z*cs[2];
}

static void stumpff_cs3(double *restrict cs, double z) {
	unsigned int n = 0;
	while(z>0.1){
		z = z/4.;
		n++;
	}
	double zm = -z;
	cs[2] = invfactorial[2] - z*invfactorial[4]; 	// always calculate first two terms
	cs[3] = invfactorial[3] - z*invfactorial[5]; 	// always calculate first two terms
	double old_c_2;
	double _pow = zm;
	unsigned int k=6;
	do{
		old_c_2 = cs[2];
		_pow *= zm;
		cs[2] += _pow*invfactorial[k];
		k+=1;
		cs[3] += _pow*invfactorial[k];
		k+=1;
	}while(cs[2]!=old_c_2 && k<34);			// Stop if new term smaller than machine precision (cs[3] converges faster than cs[2])
	cs[1] = 1.-z*cs[3];
	cs[0] = 1.-z*cs[2];
	for (;n>0;n--){	
		cs[3] = (cs[2]+cs[0]*cs[3])*0.25;
		cs[2] = cs[1]*cs[1]*0.5;
		cs[1] = cs[0]*cs[1];
		cs[0] = 2.*cs[0]*cs[0]-1.;
	}
}

static void stiefel_Gs(double *restrict Gs, double beta, double X) {
	double X2 = X*X;
	stumpff_cs(Gs, beta*X2);
	Gs[1] *= X; 
	Gs[2] *= X2; 
	double _pow = X2*X;
	Gs[3] *= _pow; 
	_pow *= X;
	Gs[4] *= _pow; 
	_pow *= X;
	Gs[5] *= _pow; 
	return;
}

static void stiefel_Gs3(double *restrict Gs, double beta, double X) {
	double X2 = X*X;
	stumpff_cs3(Gs, beta*X2);
	Gs[1] *= X; 
	Gs[2] *= X2; 
	Gs[3] *= X2*X;
	return;
}

static inline double _M(unsigned int i){
  	return G*(eta[i]); // Hanno 1
	//return G*(eta[i-1]); // Hanno2 
	//return G*(eta[i-1]*particles[i].m*eta[i-1]/eta[i]/(eta[i-1]+particles[i].m*eta[i-1]/eta[i])); // reduced mass jacobi
	//return G*(eta[i-1]*particles[i].m/(eta[i-1]+particles[i].m)); // reduced mass
	//return G*(eta[i]/eta[i-1]*particles[0].m);   // SSD
}

/****************************** 
 * Keplerian motion           */
int iter;

static void kepler_step(unsigned int i,double _dt){
	double M = _M(i);
	struct particle p1 = p_j[i];

	double r0 = sqrt(p1.x*p1.x + p1.y*p1.y + p1.z*p1.z);
	double r0i = 1./r0;
	double v2 =  p1.vx*p1.vx + p1.vy*p1.vy + p1.vz*p1.vz;
	double beta = 2.*M*r0i - v2;
	double eta0 = p1.x*p1.vx + p1.y*p1.vy + p1.z*p1.vz;
	double zeta0 = M - beta*r0;

	double X;
	double Gs[6]; 
		
	if (beta>0.){
		// Elliptic orbit
		//X = _dt*invperiod*X_per_period; // first order guess 
		double dtr0i = _dt*r0i;
		//X = dtr0i; // first order guess
		X = dtr0i * (1. - dtr0i*eta0*0.5*r0i); // second order guess
		//X = dtr0i *(1.- 0.5*dtr0i*r0i*(eta0-dtr0i*(eta0*eta0*r0i-1./3.*zeta0))); // third order guess
	}else{
		// Hyperbolic orbit
		X = 0.; // Initial guess 
	}

	unsigned int n_hg;
	unsigned int converged = 0;
	double ri;
	double oldX;
	for (n_hg=0;n_hg<10;n_hg++){
		oldX = X;
		stiefel_Gs3(Gs, beta, X);
		//double s   = r0*X + eta0*Gs[2] + zeta0*Gs[3]-_dt;
		const double eta0Gs1zeta0Gs2 = eta0*Gs[1] + zeta0*Gs[2];
		ri          = 1./(r0 + eta0Gs1zeta0Gs2);

		X = ri*(X*eta0Gs1zeta0Gs2-eta0*Gs[2]-zeta0*Gs[3]+_dt);
		

		//double dX  = -s*ri; // Newton's method
		//double spp = eta0*Gs[0] + zeta0*Gs[1];
		//double dX  = -(s*sp)/(sp*sp-0.5*s*spp); // Householder 2nd order formula
		//double s1 = r0 + eta0*Gs[1] + zeta0*Gs[2];
		//double s2 = eta0*Gs[0] + zeta0*Gs[1];
		//double s3 = -eta0*beta*Gs[1] + zeta0*Gs[0];
		
		//ri          = 1./s1; // 1./(r0 + eta0*Gs[1] + zeta0*Gs[2]);
		//double dX  = -s*ri; // Newton's method
		//double dX2 = -s/(s1 + 0.5*dX*s2); // Halley's method (3rd order)
		//double dX3 = -s/(s1 + 0.5*dX2*s2 + dX2*dX2*s3/6.); // 4th order
		 
		//X+=dX;
		//if (X>X_max || X < X_min){
		//	// Did not converged.
		//	n_hg=10;
		//	break;
		//}
		//if (fabs((X-oldX))<1e-15*fabs(X)){
		if (X==oldX){
			// Converged. Exit.
			n_hg++;
			converged = 1;
			break; 
		}
	}
	if (converged == 0){ // Fallback to bisection 
		double X_min, X_max;
		if (beta>0.){
			//Elliptic
			double sqrt_beta = sqrt(beta);
			double invperiod = sqrt_beta*beta/(2.*M_PI*M);
			double X_per_period = 2.*M_PI/sqrt_beta;
			if (dt*invperiod>1. && integrator_timestep_warning == 0){
				integrator_timestep_warning++;
				fprintf(stderr,"\n\033[1mWarning!\033[0m Timestep is larger than at least one orbital period.\n");
			}
			X_min = X_per_period * floor(_dt*invperiod);
			X_max = X_min + X_per_period;
		}else{
			//Hyperbolic
			double h2 = r0*r0*v2-eta0*eta0;
			double q = h2/M/(1.+sqrt(1.-h2*beta/(M*M)));
			double vq = sqrt(h2)/q;
			X_min = 1./(vq+r0/_dt);
			X_max = _dt/q;
		}
		X = (X_max + X_min)/2.;
		do{
			n_hg++;
			stiefel_Gs3(Gs, beta, X);
			double s   = r0*X + eta0*Gs[2] + zeta0*Gs[3]-_dt;
			if (s>=0.){
				X_max = X;
			}else{
				X_min = X;
			}
			X = (X_max + X_min)/2.;
		}while (fastabs((X_max-X_min)/X_max)>1e-15);
		ri          = 1./(r0 + eta0*Gs[1] + zeta0*Gs[2]);
	}
	
	iter += n_hg;

	double f = 1.-M*Gs[2]*r0i;
	double g = _dt - M*Gs[3];
	double fd = -M*Gs[1]*r0i*ri; 
	double gd = 1.-M*Gs[2]*ri; 

	p_j[i].x = f*p1.x + g*p1.vx;
	p_j[i].y = f*p1.y + g*p1.vy;
	p_j[i].z = f*p1.z + g*p1.vz;

	p_j[i].vx = fd*p1.x + gd*p1.vx;
	p_j[i].vy = fd*p1.y + gd*p1.vy;
	p_j[i].vz = fd*p1.z + gd*p1.vz;

	//Variations
	if (N_megno){
		stiefel_Gs(Gs, beta, X);	// Recalculate (to get Gs[4] and Gs[5])
		struct particle dp1 = p_j[i+N_megno];
		double dr0 = (dp1.x*p1.x + dp1.y*p1.y + dp1.z*p1.z)*r0i;
		double dbeta = -2.*M*dr0*r0i*r0i - 2.* (dp1.vx*p1.vx + dp1.vy*p1.vy + dp1.vz*p1.vz);
		double deta0 = dp1.x*p1.vx + dp1.y*p1.vy + dp1.z*p1.vz
			     + p1.x*dp1.vx + p1.y*dp1.vy + p1.z*dp1.vz;
		double dzeta0 = -beta*dr0 - r0*dbeta;
		double G3beta = 0.5*(3.*Gs[5]-X*Gs[4]);
		double G2beta = 0.5*(2.*Gs[4]-X*Gs[3]);
		double G1beta = 0.5*(Gs[3]-X*Gs[2]);
		double tbeta = eta0*G2beta + zeta0*G3beta;
		double dX = -1.*ri*(X*dr0 + Gs[2]*deta0+Gs[3]*dzeta0+tbeta*dbeta);
		double dG1 = Gs[0]*dX + G1beta*dbeta; 
		double dG2 = Gs[1]*dX + G2beta*dbeta;
		double dG3 = Gs[2]*dX + G3beta*dbeta;
		double dr = dr0 + Gs[1]*deta0 + Gs[2]*dzeta0 + eta0*dG1 + zeta0*dG2;
		double df = M*Gs[2]*dr0*r0i*r0i - M*dG2*r0i;
		double dg = -M*dG3;
		double dfd = -M*dG1*r0i*ri + M*Gs[1]*(dr0*r0i+dr*ri)*r0i*ri;
		double dgd = -M*dG2*ri + M*Gs[2]*dr*ri*ri;
	
		p_j[i+N_megno].x = f*dp1.x + g*dp1.vx + df*p1.x + dg*p1.vx;
		p_j[i+N_megno].y = f*dp1.y + g*dp1.vy + df*p1.y + dg*p1.vy;
		p_j[i+N_megno].z = f*dp1.z + g*dp1.vz + df*p1.z + dg*p1.vz;

		p_j[i+N_megno].vx = fd*dp1.x + gd*dp1.vx + dfd*p1.x + dgd*p1.vx;
		p_j[i+N_megno].vy = fd*dp1.y + gd*dp1.vy + dfd*p1.y + dgd*p1.vy;
		p_j[i+N_megno].vz = fd*dp1.z + gd*dp1.vz + dfd*p1.z + dgd*p1.vz;
	}

}

static void kepler_step2(unsigned int i,double _dt){
	double M = _M(i);
	struct particle p1 = p_j[i];

	double r0 = sqrt(p1.x*p1.x + p1.y*p1.y + p1.z*p1.z);
	double r0i = 1./r0;
	double v2 =  p1.vx*p1.vx + p1.vy*p1.vy + p1.vz*p1.vz;
	double beta = 2.*M*r0i - v2;
	double eta0 = p1.x*p1.vx + p1.y*p1.vy + p1.z*p1.vz;
	double zeta0 = M - beta*r0;

	double X;
	double Gs[6]; 
	double sqrt_beta = sqrt(beta);
	double period = (2.*M_PI*M)/sqrt_beta*beta;
	_dt = _dt+period;
		
	if (beta>0.){
		// Elliptic orbit
		//X = _dt*invperiod*X_per_period; // first order guess 
		double dtr0i = _dt*r0i;
		//X = dtr0i; // first order guess
		X = dtr0i * (1. - dtr0i*eta0*0.5*r0i); // second order guess
		//X = dtr0i *(1.- 0.5*dtr0i*r0i*(eta0-dtr0i*(eta0*eta0*r0i-1./3.*zeta0))); // third order guess
	}else{
		// Hyperbolic orbit
		X = 0.; // Initial guess 
	}

	unsigned int n_hg;
	unsigned int converged = 0;
	double ri;
	double oldX;
	for (n_hg=0;n_hg<10;n_hg++){
		oldX = X;
		stiefel_Gs3(Gs, beta, X);
		//double s   = r0*X + eta0*Gs[2] + zeta0*Gs[3]-_dt;
		const double eta0Gs1zeta0Gs2 = eta0*Gs[1] + zeta0*Gs[2];
		ri          = 1./(r0 + eta0Gs1zeta0Gs2);

		X = ri*(X*eta0Gs1zeta0Gs2-eta0*Gs[2]-zeta0*Gs[3]+_dt);
		

		//double dX  = -s*ri; // Newton's method
		//double spp = eta0*Gs[0] + zeta0*Gs[1];
		//double dX  = -(s*sp)/(sp*sp-0.5*s*spp); // Householder 2nd order formula
		//double s1 = r0 + eta0*Gs[1] + zeta0*Gs[2];
		//double s2 = eta0*Gs[0] + zeta0*Gs[1];
		//double s3 = -eta0*beta*Gs[1] + zeta0*Gs[0];
		
		//ri          = 1./s1; // 1./(r0 + eta0*Gs[1] + zeta0*Gs[2]);
		//double dX  = -s*ri; // Newton's method
		//double dX2 = -s/(s1 + 0.5*dX*s2); // Halley's method (3rd order)
		//double dX3 = -s/(s1 + 0.5*dX2*s2 + dX2*dX2*s3/6.); // 4th order
		 
		//X+=dX;
		//if (X>X_max || X < X_min){
		//	// Did not converged.
		//	n_hg=10;
		//	break;
		//}
		//if (fabs((X-oldX))<1e-15*fabs(X)){
		if (X==oldX){
			// Converged. Exit.
			n_hg++;
			converged = 1;
			break; 
		}
	}
	if (converged == 0){ // Fallback to bisection 
		double X_min, X_max;
		if (beta>0.){
			//Elliptic
			double sqrt_beta = sqrt(beta);
			double invperiod = sqrt_beta*beta/(2.*M_PI*M);
			double X_per_period = 2.*M_PI/sqrt_beta;
			if (dt*invperiod>1. && integrator_timestep_warning == 0){
				integrator_timestep_warning++;
				fprintf(stderr,"\n\033[1mWarning!\033[0m Timestep is larger than at least one orbital period.\n");
			}
			X_min = X_per_period * floor(_dt*invperiod);
			X_max = X_min + X_per_period;
		}else{
			//Hyperbolic
			double h2 = r0*r0*v2-eta0*eta0;
			double q = h2/M/(1.+sqrt(1.-h2*beta/(M*M)));
			double vq = sqrt(h2)/q;
			X_min = 1./(vq+r0/_dt);
			X_max = _dt/q;
		}
		X = (X_max + X_min)/2.;
		do{
			n_hg++;
			stiefel_Gs3(Gs, beta, X);
			double s   = r0*X + eta0*Gs[2] + zeta0*Gs[3]-_dt;
			if (s>=0.){
				X_max = X;
			}else{
				X_min = X;
			}
			X = (X_max + X_min)/2.;
		}while (fastabs((X_max-X_min)/X_max)>1e-15);
		ri          = 1./(r0 + eta0*Gs[1] + zeta0*Gs[2]);
	}
	
	iter += n_hg;

	double f = 1.-M*Gs[2]*r0i;
	double g = _dt - M*Gs[3];
	double fd = -M*Gs[1]*r0i*ri; 
	double gd = 1.-M*Gs[2]*ri; 

	p_j[i].x = f*p1.x + g*p1.vx;
	p_j[i].y = f*p1.y + g*p1.vy;
	p_j[i].z = f*p1.z + g*p1.vz;

	p_j[i].vx = fd*p1.x + gd*p1.vx;
	p_j[i].vy = fd*p1.y + gd*p1.vy;
	p_j[i].vz = fd*p1.z + gd*p1.vz;

	//Variations
	if (N_megno){
		stiefel_Gs(Gs, beta, X);	// Recalculate (to get Gs[4] and Gs[5])
		struct particle dp1 = p_j[i+N_megno];
		double dr0 = (dp1.x*p1.x + dp1.y*p1.y + dp1.z*p1.z)*r0i;
		double dbeta = -2.*M*dr0*r0i*r0i - 2.* (dp1.vx*p1.vx + dp1.vy*p1.vy + dp1.vz*p1.vz);
		double deta0 = dp1.x*p1.vx + dp1.y*p1.vy + dp1.z*p1.vz
			     + p1.x*dp1.vx + p1.y*dp1.vy + p1.z*dp1.vz;
		double dzeta0 = -beta*dr0 - r0*dbeta;
		double G3beta = 0.5*(3.*Gs[5]-X*Gs[4]);
		double G2beta = 0.5*(2.*Gs[4]-X*Gs[3]);
		double G1beta = 0.5*(Gs[3]-X*Gs[2]);
		double tbeta = eta0*G2beta + zeta0*G3beta;
		double dX = -1.*ri*(X*dr0 + Gs[2]*deta0+Gs[3]*dzeta0+tbeta*dbeta);
		double dG1 = Gs[0]*dX + G1beta*dbeta; 
		double dG2 = Gs[1]*dX + G2beta*dbeta;
		double dG3 = Gs[2]*dX + G3beta*dbeta;
		double dr = dr0 + Gs[1]*deta0 + Gs[2]*dzeta0 + eta0*dG1 + zeta0*dG2;
		double df = M*Gs[2]*dr0*r0i*r0i - M*dG2*r0i;
		double dg = -M*dG3;
		double dfd = -M*dG1*r0i*ri + M*Gs[1]*(dr0*r0i+dr*ri)*r0i*ri;
		double dgd = -M*dG2*ri + M*Gs[2]*dr*ri*ri;
	
		p_j[i+N_megno].x = f*dp1.x + g*dp1.vx + df*p1.x + dg*p1.vx;
		p_j[i+N_megno].y = f*dp1.y + g*dp1.vy + df*p1.y + dg*p1.vy;
		p_j[i+N_megno].z = f*dp1.z + g*dp1.vz + df*p1.z + dg*p1.vz;

		p_j[i+N_megno].vx = fd*dp1.x + gd*dp1.vx + dfd*p1.x + dgd*p1.vx;
		p_j[i+N_megno].vy = fd*dp1.y + gd*dp1.vy + dfd*p1.y + dgd*p1.vy;
		p_j[i+N_megno].vz = fd*dp1.z + gd*dp1.vz + dfd*p1.z + dgd*p1.vz;
	}

}

/****************************** 
 * Coordinate transformations */

static void integrator_to_jacobi_posvel(){
	double s_x = particles[0].m * particles[0].x;
	double s_y = particles[0].m * particles[0].y;
	double s_z = particles[0].m * particles[0].z;
	double s_vx = particles[0].m * particles[0].vx;
	double s_vy = particles[0].m * particles[0].vy;
	double s_vz = particles[0].m * particles[0].vz;
	p_j[0].m = particles[0].m;
	for (unsigned int i=1;i<N-N_megno;i++){
		const double ei = 1./eta[i-1];
		const struct particle pi = particles[i];
		p_j[i].x = pi.x - s_x*ei;
		p_j[i].y = pi.y - s_y*ei;
		p_j[i].z = pi.z - s_z*ei;
		p_j[i].vx = pi.vx - s_vx*ei;
		p_j[i].vy = pi.vy - s_vy*ei;
		p_j[i].vz = pi.vz - s_vz*ei;
		s_x  = s_x  *(1.+pi.m/eta[i-1]) + pi.m*p_j[i].x ; 
		s_y  = s_y  *(1.+pi.m/eta[i-1]) + pi.m*p_j[i].y ; 
		s_z  = s_z  *(1.+pi.m/eta[i-1]) + pi.m*p_j[i].z ; 
		s_vx = s_vx *(1.+pi.m/eta[i-1]) + pi.m*p_j[i].vx; 
		s_vy = s_vy *(1.+pi.m/eta[i-1]) + pi.m*p_j[i].vy; 
		s_vz = s_vz *(1.+pi.m/eta[i-1]) + pi.m*p_j[i].vz; 
		p_j[i].m = pi.m;
	}
	p_j[0].x = s_x * Mtotali;
	p_j[0].y = s_y * Mtotali;
	p_j[0].z = s_z * Mtotali;
	p_j[0].vx = s_vx * Mtotali;
	p_j[0].vy = s_vy * Mtotali;
	p_j[0].vz = s_vz * Mtotali;
}

static void integrator_var_to_jacobi_posvel(){
	double s_x = particles[N_megno].m * particles[N_megno].x;
	double s_y = particles[N_megno].m * particles[N_megno].y;
	double s_z = particles[N_megno].m * particles[N_megno].z;
	double s_vx = particles[N_megno].m * particles[N_megno].vx;
	double s_vy = particles[N_megno].m * particles[N_megno].vy;
	double s_vz = particles[N_megno].m * particles[N_megno].vz;
	for (unsigned int i=1+N_megno;i<N;i++){
		const double ei = 1./eta[i-1-N_megno];
		const struct particle pi = particles[i];
		p_j[i].x = pi.x - s_x*ei;
		p_j[i].y = pi.y - s_y*ei;
		p_j[i].z = pi.z - s_z*ei;
		p_j[i].vx = pi.vx - s_vx*ei;
		p_j[i].vy = pi.vy - s_vy*ei;
		p_j[i].vz = pi.vz - s_vz*ei;
		s_x += pi.m * pi.x;
		s_y += pi.m * pi.y;
		s_z += pi.m * pi.z;
		s_vx += pi.m * pi.vx;
		s_vy += pi.m * pi.vy;
		s_vz += pi.m * pi.vz;
	}
	p_j[N_megno].x = s_x * Mtotali;
	p_j[N_megno].y = s_y * Mtotali;
	p_j[N_megno].z = s_z * Mtotali;
	p_j[N_megno].vx = s_vx * Mtotali;
	p_j[N_megno].vy = s_vy * Mtotali;
	p_j[N_megno].vz = s_vz * Mtotali;
}


static void integrator_to_jacobi_acc(){
	double s_ax = particles[0].m * particles[0].ax;
	double s_ay = particles[0].m * particles[0].ay;
	double s_az = particles[0].m * particles[0].az;
	p_j[0].m = particles[0].m;
	for (unsigned int i=1;i<N-N_megno;i++){
		const double ei = 1./eta[i-1];
		const struct particle pi = particles[i];
		p_j[i].ax = pi.ax - s_ax*ei;
		p_j[i].ay = pi.ay - s_ay*ei;
		p_j[i].az = pi.az - s_az*ei;
		s_ax = s_ax *(1.+pi.m/eta[i-1]) + pi.m*p_j[i].ax; 
		s_ay = s_ay *(1.+pi.m/eta[i-1]) + pi.m*p_j[i].ay; 
		s_az = s_az *(1.+pi.m/eta[i-1]) + pi.m*p_j[i].az; 
		p_j[i].m = pi.m;
	}
	p_j[0].ax = s_ax * Mtotali;
	p_j[0].ay = s_ay * Mtotali;
	p_j[0].az = s_az * Mtotali;
}

static void integrator_var_to_jacobi_acc(){
	double s_ax = particles[N_megno].m * particles[N_megno].ax;
	double s_ay = particles[N_megno].m * particles[N_megno].ay;
	double s_az = particles[N_megno].m * particles[N_megno].az;
	for (unsigned int i=1+N_megno;i<N;i++){
		const double ei = 1./eta[i-1-N_megno];
		const struct particle pi = particles[i];
		p_j[i].ax = pi.ax - s_ax*ei;
		p_j[i].ay = pi.ay - s_ay*ei;
		p_j[i].az = pi.az - s_az*ei;
		s_ax += pi.m * pi.ax;
		s_ay += pi.m * pi.ay;
		s_az += pi.m * pi.az;
	}
	p_j[N_megno].ax = s_ax * Mtotali;
	p_j[N_megno].ay = s_ay * Mtotali;
	p_j[N_megno].az = s_az * Mtotali;
}

static void integrator_to_heliocentric_posvel(){
	double s_x  = p_j[0].x / Mtotali; 
	double s_y  = p_j[0].y / Mtotali; 
	double s_z  = p_j[0].z / Mtotali; 
	double s_vx = p_j[0].vx / Mtotali; 
	double s_vy = p_j[0].vy / Mtotali; 
	double s_vz = p_j[0].vz / Mtotali; 
	for (unsigned int i=N-N_megno-1;i>0;i--){
		const struct particle pji = p_j[i];
		// s_(i-1)
		s_x  = (s_x  - pji.m * pji.x ) *eta[i-1]/eta[i];
		s_y  = (s_y  - pji.m * pji.y ) *eta[i-1]/eta[i];
		s_z  = (s_z  - pji.m * pji.z ) *eta[i-1]/eta[i];
		s_vx = (s_vx - pji.m * pji.vx) *eta[i-1]/eta[i];
		s_vy = (s_vy - pji.m * pji.vy) *eta[i-1]/eta[i];
		s_vz = (s_vz - pji.m * pji.vz) *eta[i-1]/eta[i];
		particles[i].x  = pji.x  + s_x /eta[i-1];
		particles[i].y  = pji.y  + s_y /eta[i-1];
		particles[i].z  = pji.z  + s_z /eta[i-1];
		particles[i].vx = pji.vx + s_vx/eta[i-1];
		particles[i].vy = pji.vy + s_vy/eta[i-1];
		particles[i].vz = pji.vz + s_vz/eta[i-1];
	}
	particles[0].x = s_x / particles[0].m;
	particles[0].y = s_y / particles[0].m;
	particles[0].z = s_z / particles[0].m;
	particles[0].vx = s_vx / particles[0].m;
	particles[0].vy = s_vy / particles[0].m;
	particles[0].vz = s_vz / particles[0].m;
//	double s_x = 0.;
//	double s_y = 0.;
//	double s_z = 0.;
//	double s_vx = 0.;
//	double s_vy = 0.;
//	double s_vz = 0.;
//	for (unsigned int i=N-N_megno-1;i>0;i--){
//		struct particle pji = p_j[i];
//		const double ei = 1./eta[i];
//		const double pjimei = pji.m*ei;
//		const double ee = 1./(eta[i-1]/pji.m+1.);
//		// Do not attempt to simply the expressions below. Simplifying them would result in a biased error growth.
//		particles[i].x  = pji.x  + p_j[0].x  - ee * pji.x  - s_x;
//		particles[i].y  = pji.y  + p_j[0].y  - ee * pji.y  - s_y;
//		particles[i].z  = pji.z  + p_j[0].z  - ee * pji.z  - s_z;
//		particles[i].vx = pji.vx + p_j[0].vx - ee * pji.vx - s_vx;
//		particles[i].vy = pji.vy + p_j[0].vy - ee * pji.vy - s_vy;
//		particles[i].vz = pji.vz + p_j[0].vz - ee * pji.vz - s_vz;
//		s_x  += pjimei * pji.x;
//		s_y  += pjimei * pji.y;
//		s_z  += pjimei * pji.z;
//		s_vx += pjimei * pji.vx;
//		s_vy += pjimei * pji.vy;
//		s_vz += pjimei * pji.vz;
//	}
//	particles[0].x = p_j[0].x - s_x;
//	particles[0].y = p_j[0].y - s_y;
//	particles[0].z = p_j[0].z - s_z;
//	particles[0].vx = p_j[0].vx - s_vx;
//	particles[0].vy = p_j[0].vy - s_vy;
//	particles[0].vz = p_j[0].vz - s_vz;
}

static void integrator_var_to_heliocentric_posvel(){
	double s_x = 0.;
	double s_y = 0.;
	double s_z = 0.;
	double s_vx = 0.;
	double s_vy = 0.;
	double s_vz = 0.;
	for (unsigned int i=N-1;i>N_megno;i--){
		const double ee = 1./(eta[i-1-N_megno]/particles[i-N_megno].m+1.);
		// Do not attempt to simply the expressions below. Simplifying them would result in a biased error growth.
		const double ei = 1./eta[i-N_megno];
		struct particle pji = p_j[i];
		particles[i].x  = pji.x  + p_j[N_megno].x  - ee * pji.x - s_x;
		particles[i].y  = pji.y  + p_j[N_megno].y  - ee * pji.y - s_y;
		particles[i].z  = pji.z  + p_j[N_megno].z  - ee * pji.z - s_z;
		particles[i].vx = pji.vx + p_j[N_megno].vx - ee * pji.vx - s_vx;
		particles[i].vy = pji.vy + p_j[N_megno].vy - ee * pji.vy - s_vy;
		particles[i].vz = pji.vz + p_j[N_megno].vz - ee * pji.vz - s_vz;
		s_x += pji.m*ei * pji.x;
		s_y += pji.m*ei * pji.y;
		s_z += pji.m*ei * pji.z;
		s_vx += pji.m*ei * pji.vx;
		s_vy += pji.m*ei * pji.vy;
		s_vz += pji.m*ei * pji.vz;
	}
	particles[N_megno].x = p_j[N_megno].x - s_x;
	particles[N_megno].y = p_j[N_megno].y - s_y;
	particles[N_megno].z = p_j[N_megno].z - s_z;
	particles[N_megno].vx = p_j[N_megno].vx - s_vx;
	particles[N_megno].vy = p_j[N_megno].vy - s_vy;
	particles[N_megno].vz = p_j[N_megno].vz - s_vz;
}

static void integrator_to_heliocentric_pos(){
	double s_x = 0.;
	double s_y = 0.;
	double s_z = 0.;
	for (unsigned int i=N-N_megno-1;i>0;i--){
		const double ei = 1./eta[i];
		const struct particle pji = p_j[i];
		const double ee = 1./(eta[i-1]/pji.m+1.);
		// Do not attempt to simply the expressions below. Simplifying them would result in a biased error growth.
		particles[i].x  = pji.x  + p_j[0].x  - ee * pji.x  - s_x;
		particles[i].y  = pji.y  + p_j[0].y  - ee * pji.y  - s_y;
		particles[i].z  = pji.z  + p_j[0].z  - ee * pji.z  - s_z;
		s_x += pji.m*ei * pji.x;
		s_y += pji.m*ei * pji.y;
		s_z += pji.m*ei * pji.z;
	}
	particles[0].x = p_j[0].x - s_x;
	particles[0].y = p_j[0].y - s_y;
	particles[0].z = p_j[0].z - s_z;
}
static void integrator_var_to_heliocentric_pos(){
	double s_x = 0.;
	double s_y = 0.;
	double s_z = 0.;
	for (unsigned int i=N-1;i>N_megno;i--){
		const double ei = 1./eta[i-N_megno];
		const double ee = 1./(eta[i-1-N_megno]/particles[i-N_megno].m+1.);
		const struct particle pji = p_j[i];
		particles[i].x  = pji.x  + p_j[N_megno].x  - ee * pji.x - s_x;
		particles[i].y  = pji.y  + p_j[N_megno].y  - ee * pji.y - s_y;
		particles[i].z  = pji.z  + p_j[N_megno].z  - ee * pji.z - s_z;
		s_x += pji.m*ei * pji.x;
		s_y += pji.m*ei * pji.y;
		s_z += pji.m*ei * pji.z;
	}
	particles[N_megno].x = p_j[N_megno].x - s_x;
	particles[N_megno].y = p_j[N_megno].y - s_y;
	particles[N_megno].z = p_j[N_megno].z - s_z;
}

/***************************** 
 * Interaction Hamiltonian  */

static void integrator_interaction(double _dt){
	for (unsigned int i=1;i<N-N_megno;i++){
		// Eq 132
		struct particle pji = p_j[i];
		double rj  = pow(pji.x*pji.x + pji.y*pji.y + pji.z*pji.z,-1./2.);
		double rj3 = rj*rj*rj;
		double M = _M(i);
		double prefac1 = _dt*M*rj3;
		p_j[i].vx += _dt * pji.ax;
		p_j[i].vy += _dt * pji.ay;
		p_j[i].vz += _dt * pji.az;
		if (i>1){
			p_j[i].vx += prefac1*pji.x;
			p_j[i].vy += prefac1*pji.y;
			p_j[i].vz += prefac1*pji.z;
		}
		if (N_megno){
			// Eq 132
			double rj5 = rj3*rj*rj;
			double rdr = p_j[i+N_megno].x*pji.x + p_j[i+N_megno].y*pji.y + p_j[i+N_megno].z*pji.z;
			double prefac2 = -_dt*M*3.*rdr*rj5;
			p_j[i+N_megno].vx += _dt * p_j[i+N_megno].ax;
			p_j[i+N_megno].vy += _dt * p_j[i+N_megno].ay;
			p_j[i+N_megno].vz += _dt * p_j[i+N_megno].az;
			if (i>1){
				p_j[i+N_megno].vx += prefac1*p_j[i+N_megno].x + prefac2*pji.x;
				p_j[i+N_megno].vy += prefac1*p_j[i+N_megno].y + prefac2*pji.y;
				p_j[i+N_megno].vz += prefac1*p_j[i+N_megno].z + prefac2*pji.z;
			}
		}
	}
}

unsigned int integrator_allocated_N = 0;
void integrator_mikkola_init(){
	if (integrator_allocated_N != N){
		integrator_allocated_N = N;
		p_j = realloc(p_j,sizeof(struct particle)*N);
		eta = realloc(eta,sizeof(double)*(N-N_megno));
	}
}
/***************************** 
 * KDK Scheme                */
void integrator_part1(){
	integrator_mikkola_init();
	eta[0] = particles[0].m;
	for (unsigned int i=1;i<N-N_megno;i++){
		eta[i] = eta[i-1] + particles[i].m;
	}
	Mtotali = 1./eta[N-N_megno-1];
	integrator_to_jacobi_posvel();
	if (N_megno){
		integrator_var_to_jacobi_posvel();
	}

	for (unsigned int i=1;i<N-N_megno;i++){
		struct particle b = p_j[i];
		kepler_step(i, dt/2.);
		struct particle a = p_j[i];
		p_j[i] = b;
		kepler_step2(i, dt/2.);
		p_j[i].x = (p_j[i].x + a.x)/2.;
		p_j[i].y = (p_j[i].y + a.y)/2.;
		p_j[i].z = (p_j[i].z + a.z)/2.;
		p_j[i].vx = (p_j[i].vx + a.vx)/2.;
		p_j[i].vy = (p_j[i].vy + a.vy)/2.;
		p_j[i].vz = (p_j[i].vz + a.vz)/2.;

	}
	p_j[0].x += dt/2.*p_j[0].vx;
	p_j[0].y += dt/2.*p_j[0].vy;
	p_j[0].z += dt/2.*p_j[0].vz;
	if (integrator_force_is_velocitydependent){
		integrator_to_heliocentric_posvel();
	}else{
		integrator_to_heliocentric_pos();
	}
	
	if (N_megno){
		p_j[N_megno].x += dt/2.*p_j[N_megno].vx;
		p_j[N_megno].y += dt/2.*p_j[N_megno].vy;
		p_j[N_megno].z += dt/2.*p_j[N_megno].vz;
		if (integrator_force_is_velocitydependent){
			integrator_var_to_heliocentric_posvel();
		}else{
			integrator_var_to_heliocentric_pos();
		}
	}

	t+=dt/2.;
}

void integrator_part2(){
	integrator_to_jacobi_acc();
	if (N_megno){
		integrator_var_to_jacobi_acc();
	}
	integrator_interaction(dt);

	for (unsigned int i=1;i<N-N_megno;i++){
		struct particle b = p_j[i];
		kepler_step(i, dt/2.);
		struct particle a = p_j[i];
		p_j[i] = b;
		kepler_step2(i, dt/2.);
		p_j[i].x = (p_j[i].x + a.x)/2.;
		p_j[i].y = (p_j[i].y + a.y)/2.;
		p_j[i].z = (p_j[i].z + a.z)/2.;
		p_j[i].vx = (p_j[i].vx + a.vx)/2.;
		p_j[i].vy = (p_j[i].vy + a.vy)/2.;
		p_j[i].vz = (p_j[i].vz + a.vz)/2.;

	}
	p_j[0].x += dt/2.*p_j[0].vx;
	p_j[0].y += dt/2.*p_j[0].vy;
	p_j[0].z += dt/2.*p_j[0].vz;
	integrator_to_heliocentric_posvel();
	
	if (N_megno){
		p_j[N_megno].x += dt/2.*p_j[N_megno].vx;
		p_j[N_megno].y += dt/2.*p_j[N_megno].vy;
		p_j[N_megno].z += dt/2.*p_j[N_megno].vz;
		integrator_var_to_heliocentric_posvel();
	}

	t+=dt/2.;
	
	if (N_megno){
		double dY = dt * 2. * t * tools_megno_deltad_delta();
		tools_megno_update(dY);
	}
}
	

void integrator_reset(){
	integrator_allocated_N = 0;
	free(p_j);
	p_j = NULL;
	free(eta);
	eta = NULL;
}
