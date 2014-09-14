/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/implicit.c
 *  \ingroup bph
 */

#include "implicit.h"

#ifdef IMPLICIT_SOLVER_BLENDER

#include "MEM_guardedalloc.h"

#include "DNA_scene_types.h"
#include "DNA_object_types.h"
#include "DNA_object_force.h"
#include "DNA_meshdata_types.h"
#include "DNA_texture_types.h"

#include "BLI_math.h"
#include "BLI_linklist.h"
#include "BLI_utildefines.h"

#include "BKE_cloth.h"
#include "BKE_collision.h"
#include "BKE_effect.h"
#include "BKE_global.h"

#include "BPH_mass_spring.h"

#ifdef __GNUC__
#  pragma GCC diagnostic ignored "-Wtype-limits"
#endif

#ifdef _OPENMP
#  define CLOTH_OPENMP_LIMIT 512
#endif

#if 0  /* debug timing */
#ifdef _WIN32
#include <windows.h>
static LARGE_INTEGER _itstart, _itend;
static LARGE_INTEGER ifreq;
static void itstart(void)
{
	static int first = 1;
	if (first) {
		QueryPerformanceFrequency(&ifreq);
		first = 0;
	}
	QueryPerformanceCounter(&_itstart);
}
static void itend(void)
{
	QueryPerformanceCounter(&_itend);
}
double itval(void)
{
	return ((double)_itend.QuadPart -
			(double)_itstart.QuadPart)/((double)ifreq.QuadPart);
}
#else
#include <sys/time.h>
// intrinsics need better compile flag checking
// #include <xmmintrin.h>
// #include <pmmintrin.h>
// #include <pthread.h>

static struct timeval _itstart, _itend;
static struct timezone itz;
static void itstart(void)
{
	gettimeofday(&_itstart, &itz);
}
static void itend(void)
{
	gettimeofday(&_itend, &itz);
}
static double itval(void)
{
	double t1, t2;
	t1 =  (double)_itstart.tv_sec + (double)_itstart.tv_usec/(1000*1000);
	t2 =  (double)_itend.tv_sec + (double)_itend.tv_usec/(1000*1000);
	return t2-t1;
}
#endif
#endif  /* debug timing */

static float I[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static float ZERO[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};

/*
#define C99
#ifdef C99
#defineDO_INLINE inline 
#else 
#defineDO_INLINE static 
#endif
*/
struct Cloth;

//////////////////////////////////////////
/* fast vector / matrix library, enhancements are welcome :) -dg */
/////////////////////////////////////////

/* DEFINITIONS */
typedef float lfVector[3];
typedef struct fmatrix3x3 {
	float m[3][3]; /* 3x3 matrix */
	unsigned int c, r; /* column and row number */
	/* int pinned; // is this vertex allowed to move? */
	float n1, n2, n3; /* three normal vectors for collision constrains */
	unsigned int vcount; /* vertex count */
	unsigned int scount; /* spring count */ 
} fmatrix3x3;

///////////////////////////
// float[3] vector
///////////////////////////
/* simple vector code */
/* STATUS: verified */
DO_INLINE void mul_fvector_S(float to[3], float from[3], float scalar)
{
	to[0] = from[0] * scalar;
	to[1] = from[1] * scalar;
	to[2] = from[2] * scalar;
}
/* simple v^T * v product ("outer product") */
/* STATUS: HAS TO BE verified (*should* work) */
DO_INLINE void mul_fvectorT_fvector(float to[3][3], float vectorA[3], float vectorB[3])
{
	mul_fvector_S(to[0], vectorB, vectorA[0]);
	mul_fvector_S(to[1], vectorB, vectorA[1]);
	mul_fvector_S(to[2], vectorB, vectorA[2]);
}
/* simple v^T * v product with scalar ("outer product") */
/* STATUS: HAS TO BE verified (*should* work) */
DO_INLINE void mul_fvectorT_fvectorS(float to[3][3], float vectorA[3], float vectorB[3], float aS)
{	
	mul_fvectorT_fvector(to, vectorA, vectorB);
	
	mul_fvector_S(to[0], to[0], aS);
	mul_fvector_S(to[1], to[1], aS);
	mul_fvector_S(to[2], to[2], aS);
}

#if 0
/* printf vector[3] on console: for debug output */
static void print_fvector(float m3[3])
{
	printf("%f\n%f\n%f\n\n", m3[0], m3[1], m3[2]);
}

///////////////////////////
// long float vector float (*)[3]
///////////////////////////
/* print long vector on console: for debug output */
DO_INLINE void print_lfvector(float (*fLongVector)[3], unsigned int verts)
{
	unsigned int i = 0;
	for (i = 0; i < verts; i++) {
		print_fvector(fLongVector[i]);
	}
}
#endif

/* create long vector */
DO_INLINE lfVector *create_lfvector(unsigned int verts)
{
	/* TODO: check if memory allocation was successful */
	return  (lfVector *)MEM_callocN(verts * sizeof(lfVector), "cloth_implicit_alloc_vector");
	// return (lfVector *)cloth_aligned_malloc(&MEMORY_BASE, verts * sizeof(lfVector));
}
/* delete long vector */
DO_INLINE void del_lfvector(float (*fLongVector)[3])
{
	if (fLongVector != NULL) {
		MEM_freeN(fLongVector);
		// cloth_aligned_free(&MEMORY_BASE, fLongVector);
	}
}
/* copy long vector */
DO_INLINE void cp_lfvector(float (*to)[3], float (*from)[3], unsigned int verts)
{
	memcpy(to, from, verts * sizeof(lfVector));
}
/* init long vector with float[3] */
DO_INLINE void init_lfvector(float (*fLongVector)[3], float vector[3], unsigned int verts)
{
	unsigned int i = 0;
	for (i = 0; i < verts; i++) {
		copy_v3_v3(fLongVector[i], vector);
	}
}
/* zero long vector with float[3] */
DO_INLINE void zero_lfvector(float (*to)[3], unsigned int verts)
{
	memset(to, 0.0f, verts * sizeof(lfVector));
}
/* multiply long vector with scalar*/
DO_INLINE void mul_lfvectorS(float (*to)[3], float (*fLongVector)[3], float scalar, unsigned int verts)
{
	unsigned int i = 0;

	for (i = 0; i < verts; i++) {
		mul_fvector_S(to[i], fLongVector[i], scalar);
	}
}
/* multiply long vector with scalar*/
/* A -= B * float */
DO_INLINE void submul_lfvectorS(float (*to)[3], float (*fLongVector)[3], float scalar, unsigned int verts)
{
	unsigned int i = 0;
	for (i = 0; i < verts; i++) {
		VECSUBMUL(to[i], fLongVector[i], scalar);
	}
}
/* dot product for big vector */
DO_INLINE float dot_lfvector(float (*fLongVectorA)[3], float (*fLongVectorB)[3], unsigned int verts)
{
	long i = 0;
	float temp = 0.0;
// XXX brecht, disabled this for now (first schedule line was already disabled),
// due to non-commutative nature of floating point ops this makes the sim give
// different results each time you run it!
// schedule(guided, 2)
//#pragma omp parallel for reduction(+: temp) if (verts > CLOTH_OPENMP_LIMIT)
	for (i = 0; i < (long)verts; i++) {
		temp += dot_v3v3(fLongVectorA[i], fLongVectorB[i]);
	}
	return temp;
}
/* A = B + C  --> for big vector */
DO_INLINE void add_lfvector_lfvector(float (*to)[3], float (*fLongVectorA)[3], float (*fLongVectorB)[3], unsigned int verts)
{
	unsigned int i = 0;

	for (i = 0; i < verts; i++) {
		VECADD(to[i], fLongVectorA[i], fLongVectorB[i]);
	}

}
/* A = B + C * float --> for big vector */
DO_INLINE void add_lfvector_lfvectorS(float (*to)[3], float (*fLongVectorA)[3], float (*fLongVectorB)[3], float bS, unsigned int verts)
{
	unsigned int i = 0;

	for (i = 0; i < verts; i++) {
		VECADDS(to[i], fLongVectorA[i], fLongVectorB[i], bS);

	}
}
/* A = B * float + C * float --> for big vector */
DO_INLINE void add_lfvectorS_lfvectorS(float (*to)[3], float (*fLongVectorA)[3], float aS, float (*fLongVectorB)[3], float bS, unsigned int verts)
{
	unsigned int i = 0;

	for (i = 0; i < verts; i++) {
		VECADDSS(to[i], fLongVectorA[i], aS, fLongVectorB[i], bS);
	}
}
/* A = B - C * float --> for big vector */
DO_INLINE void sub_lfvector_lfvectorS(float (*to)[3], float (*fLongVectorA)[3], float (*fLongVectorB)[3], float bS, unsigned int verts)
{
	unsigned int i = 0;
	for (i = 0; i < verts; i++) {
		VECSUBS(to[i], fLongVectorA[i], fLongVectorB[i], bS);
	}

}
/* A = B - C --> for big vector */
DO_INLINE void sub_lfvector_lfvector(float (*to)[3], float (*fLongVectorA)[3], float (*fLongVectorB)[3], unsigned int verts)
{
	unsigned int i = 0;

	for (i = 0; i < verts; i++) {
		sub_v3_v3v3(to[i], fLongVectorA[i], fLongVectorB[i]);
	}

}
///////////////////////////
// 3x3 matrix
///////////////////////////
#if 0
/* printf 3x3 matrix on console: for debug output */
static void print_fmatrix(float m3[3][3])
{
	printf("%f\t%f\t%f\n", m3[0][0], m3[0][1], m3[0][2]);
	printf("%f\t%f\t%f\n", m3[1][0], m3[1][1], m3[1][2]);
	printf("%f\t%f\t%f\n\n", m3[2][0], m3[2][1], m3[2][2]);
}

static void print_sparse_matrix(fmatrix3x3 *m)
{
	if (m) {
		unsigned int i;
		for (i = 0; i < m[0].vcount + m[0].scount; i++) {
			printf("%d:\n", i);
			print_fmatrix(m[i].m);
		}
	}
}
#endif

static void print_lvector(lfVector *v, int numverts)
{
	int i;
	for (i = 0; i < numverts; ++i) {
		if (i > 0)
			printf("\n");
		
		printf("%f,\n", v[i][0]);
		printf("%f,\n", v[i][1]);
		printf("%f,\n", v[i][2]);
	}
}

static void print_bfmatrix(fmatrix3x3 *m)
{
	int tot = m[0].vcount + m[0].scount;
	int size = m[0].vcount * 3;
	float *t = MEM_callocN(sizeof(float) * size*size, "bfmatrix");
	int q, i, j;
	
	for (q = 0; q < tot; ++q) {
		int k = 3 * m[q].r;
		int l = 3 * m[q].c;
		
		for (j = 0; j < 3; ++j) {
			for (i = 0; i < 3; ++i) {
//				if (t[k + i + (l + j) * size] != 0.0f) {
//					printf("warning: overwriting value at %d, %d\n", m[q].r, m[q].c);
//				}
				if (k == l) {
					t[k + i + (k + j) * size] += m[q].m[i][j];
				}
				else {
					t[k + i + (l + j) * size] += m[q].m[i][j];
					t[l + j + (k + i) * size] += m[q].m[j][i];
				}
			}
		}
	}
	
	for (j = 0; j < size; ++j) {
		if (j > 0 && j % 3 == 0)
			printf("\n");
		
		for (i = 0; i < size; ++i) {
			if (i > 0 && i % 3 == 0)
				printf("  ");
			
			implicit_print_matrix_elem(t[i + j * size]);
		}
		printf("\n");
	}
	
	MEM_freeN(t);
}

/* copy 3x3 matrix */
DO_INLINE void cp_fmatrix(float to[3][3], float from[3][3])
{
	// memcpy(to, from, sizeof (float) * 9);
	copy_v3_v3(to[0], from[0]);
	copy_v3_v3(to[1], from[1]);
	copy_v3_v3(to[2], from[2]);
}

/* copy 3x3 matrix */
DO_INLINE void initdiag_fmatrixS(float to[3][3], float aS)
{
	cp_fmatrix(to, ZERO);
	
	to[0][0] = aS;
	to[1][1] = aS;
	to[2][2] = aS;
}

#if 0
/* calculate determinant of 3x3 matrix */
DO_INLINE float det_fmatrix(float m[3][3])
{
	return  m[0][0]*m[1][1]*m[2][2] + m[1][0]*m[2][1]*m[0][2] + m[0][1]*m[1][2]*m[2][0] 
			-m[0][0]*m[1][2]*m[2][1] - m[0][1]*m[1][0]*m[2][2] - m[2][0]*m[1][1]*m[0][2];
}

DO_INLINE void inverse_fmatrix(float to[3][3], float from[3][3])
{
	unsigned int i, j;
	float d;

	if ((d=det_fmatrix(from)) == 0) {
		printf("can't build inverse");
		exit(0);
	}
	for (i=0;i<3;i++) {
		for (j=0;j<3;j++) {
			int i1=(i+1)%3;
			int i2=(i+2)%3;
			int j1=(j+1)%3;
			int j2=(j+2)%3;
			// reverse indexs i&j to take transpose
			to[j][i] = (from[i1][j1]*from[i2][j2]-from[i1][j2]*from[i2][j1])/d;
			/*
			if (i==j)
			to[i][j] = 1.0f / from[i][j];
			else
			to[i][j] = 0;
			*/
		}
	}

}
#endif

/* 3x3 matrix multiplied by a scalar */
/* STATUS: verified */
DO_INLINE void mul_fmatrix_S(float matrix[3][3], float scalar)
{
	mul_fvector_S(matrix[0], matrix[0], scalar);
	mul_fvector_S(matrix[1], matrix[1], scalar);
	mul_fvector_S(matrix[2], matrix[2], scalar);
}

/* a vector multiplied by a 3x3 matrix */
/* STATUS: verified */
DO_INLINE void mul_fvector_fmatrix(float *to, float *from, float matrix[3][3])
{
	to[0] = matrix[0][0]*from[0] + matrix[1][0]*from[1] + matrix[2][0]*from[2];
	to[1] = matrix[0][1]*from[0] + matrix[1][1]*from[1] + matrix[2][1]*from[2];
	to[2] = matrix[0][2]*from[0] + matrix[1][2]*from[1] + matrix[2][2]*from[2];
}

/* 3x3 matrix multiplied by a vector */
/* STATUS: verified */
DO_INLINE void mul_fmatrix_fvector(float *to, float matrix[3][3], float from[3])
{
	to[0] = dot_v3v3(matrix[0], from);
	to[1] = dot_v3v3(matrix[1], from);
	to[2] = dot_v3v3(matrix[2], from);
}
/* 3x3 matrix addition with 3x3 matrix */
DO_INLINE void add_fmatrix_fmatrix(float to[3][3], float matrixA[3][3], float matrixB[3][3])
{
	VECADD(to[0], matrixA[0], matrixB[0]);
	VECADD(to[1], matrixA[1], matrixB[1]);
	VECADD(to[2], matrixA[2], matrixB[2]);
}
/* A -= B*x + C*y (3x3 matrix sub-addition with 3x3 matrix) */
DO_INLINE void subadd_fmatrixS_fmatrixS(float to[3][3], float matrixA[3][3], float aS, float matrixB[3][3], float bS)
{
	VECSUBADDSS(to[0], matrixA[0], aS, matrixB[0], bS);
	VECSUBADDSS(to[1], matrixA[1], aS, matrixB[1], bS);
	VECSUBADDSS(to[2], matrixA[2], aS, matrixB[2], bS);
}
/* A = B - C (3x3 matrix subtraction with 3x3 matrix) */
DO_INLINE void sub_fmatrix_fmatrix(float to[3][3], float matrixA[3][3], float matrixB[3][3])
{
	sub_v3_v3v3(to[0], matrixA[0], matrixB[0]);
	sub_v3_v3v3(to[1], matrixA[1], matrixB[1]);
	sub_v3_v3v3(to[2], matrixA[2], matrixB[2]);
}
/////////////////////////////////////////////////////////////////
// special functions
/////////////////////////////////////////////////////////////////
/* 3x3 matrix multiplied+added by a vector */
/* STATUS: verified */
DO_INLINE void muladd_fmatrix_fvector(float to[3], float matrix[3][3], float from[3])
{
	to[0] += dot_v3v3(matrix[0], from);
	to[1] += dot_v3v3(matrix[1], from);
	to[2] += dot_v3v3(matrix[2], from);
}
/////////////////////////////////////////////////////////////////

///////////////////////////
// SPARSE SYMMETRIC big matrix with 3x3 matrix entries
///////////////////////////
/* printf a big matrix on console: for debug output */
#if 0
static void print_bfmatrix(fmatrix3x3 *m3)
{
	unsigned int i = 0;

	for (i = 0; i < m3[0].vcount + m3[0].scount; i++)
	{
		print_fmatrix(m3[i].m);
	}
}
#endif

BLI_INLINE void init_fmatrix(fmatrix3x3 *matrix, int r, int c)
{
	matrix->r = r;
	matrix->c = c;
}

/* create big matrix */
DO_INLINE fmatrix3x3 *create_bfmatrix(unsigned int verts, unsigned int springs)
{
	// TODO: check if memory allocation was successful */
	fmatrix3x3 *temp = (fmatrix3x3 *)MEM_callocN(sizeof(fmatrix3x3) * (verts + springs), "cloth_implicit_alloc_matrix");
	int i;
	
	temp[0].vcount = verts;
	temp[0].scount = springs;
	
	/* vertex part of the matrix is diagonal blocks */
	for (i = 0; i < verts; ++i) {
		init_fmatrix(temp + i, i, i);
	}
	
	return temp;
}
/* delete big matrix */
DO_INLINE void del_bfmatrix(fmatrix3x3 *matrix)
{
	if (matrix != NULL) {
		MEM_freeN(matrix);
	}
}

/* copy big matrix */
DO_INLINE void cp_bfmatrix(fmatrix3x3 *to, fmatrix3x3 *from)
{
	// TODO bounds checking
	memcpy(to, from, sizeof(fmatrix3x3) * (from[0].vcount+from[0].scount));
}

/* init big matrix */
// slow in parallel
DO_INLINE void init_bfmatrix(fmatrix3x3 *matrix, float m3[3][3])
{
	unsigned int i;

	for (i = 0; i < matrix[0].vcount+matrix[0].scount; i++) {
		cp_fmatrix(matrix[i].m, m3); 
	}
}

/* init the diagonal of big matrix */
// slow in parallel
DO_INLINE void initdiag_bfmatrix(fmatrix3x3 *matrix, float m3[3][3])
{
	unsigned int i, j;
	float tmatrix[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};

	for (i = 0; i < matrix[0].vcount; i++) {
		cp_fmatrix(matrix[i].m, m3); 
	}
	for (j = matrix[0].vcount; j < matrix[0].vcount+matrix[0].scount; j++) {
		cp_fmatrix(matrix[j].m, tmatrix); 
	}
}

/* SPARSE SYMMETRIC multiply big matrix with long vector*/
/* STATUS: verified */
DO_INLINE void mul_bfmatrix_lfvector( float (*to)[3], fmatrix3x3 *from, lfVector *fLongVector)
{
	unsigned int i = 0;
	unsigned int vcount = from[0].vcount;
	lfVector *temp = create_lfvector(vcount);
	
	zero_lfvector(to, vcount);

#pragma omp parallel sections private(i) if (vcount > CLOTH_OPENMP_LIMIT)
	{
#pragma omp section
		{
			for (i = from[0].vcount; i < from[0].vcount+from[0].scount; i++) {
				muladd_fmatrix_fvector(to[from[i].c], from[i].m, fLongVector[from[i].r]);
			}
		}
#pragma omp section
		{
			for (i = 0; i < from[0].vcount+from[0].scount; i++) {
				muladd_fmatrix_fvector(temp[from[i].r], from[i].m, fLongVector[from[i].c]);
			}
		}
	}
	add_lfvector_lfvector(to, to, temp, from[0].vcount);
	
	del_lfvector(temp);
	
	
}

/* SPARSE SYMMETRIC sub big matrix with big matrix*/
/* A -= B * float + C * float --> for big matrix */
/* VERIFIED */
DO_INLINE void subadd_bfmatrixS_bfmatrixS( fmatrix3x3 *to, fmatrix3x3 *from, float aS,  fmatrix3x3 *matrix, float bS)
{
	unsigned int i = 0;

	/* process diagonal elements */
	for (i = 0; i < matrix[0].vcount+matrix[0].scount; i++) {
		subadd_fmatrixS_fmatrixS(to[i].m, from[i].m, aS, matrix[i].m, bS);
	}

}

///////////////////////////////////////////////////////////////////
// simulator start
///////////////////////////////////////////////////////////////////
typedef struct RootTransform {
	float loc[3];
	float rot[3][3];
	
	float vel[3];
	float omega[3];
	
	float acc[3];
	float domega_dt[3];
} RootTransform;

typedef struct Implicit_Data  {
	/* inputs */
	fmatrix3x3 *bigI;			/* identity (constant) */
	fmatrix3x3 *M;				/* masses */
	lfVector *F;				/* forces */
	fmatrix3x3 *dFdV, *dFdX;	/* force jacobians */
	RootTransform *root;		/* root transforms */
	
	/* motion state data */
	lfVector *X, *Xnew;			/* positions */
	lfVector *V, *Vnew;			/* velocities */
	
	/* internal solver data */
	lfVector *B;				/* B for A*dV = B */
	fmatrix3x3 *A;				/* A for A*dV = B */
	
	lfVector *dV;				/* velocity change (solution of A*dV = B) */
	lfVector *z;				/* target velocity in constrained directions */
	fmatrix3x3 *S;				/* filtering matrix for constraints */
	fmatrix3x3 *P, *Pinv;		/* pre-conditioning matrix */
} Implicit_Data;

Implicit_Data *BPH_mass_spring_solver_create(int numverts, int numsprings)
{
	Implicit_Data *id = (Implicit_Data *)MEM_callocN(sizeof(Implicit_Data), "implicit vecmat");
	
	/* process diagonal elements */
	id->A = create_bfmatrix(numverts, numsprings);
	id->dFdV = create_bfmatrix(numverts, numsprings);
	id->dFdX = create_bfmatrix(numverts, numsprings);
	id->S = create_bfmatrix(numverts, 0);
	id->Pinv = create_bfmatrix(numverts, numsprings);
	id->P = create_bfmatrix(numverts, numsprings);
	id->bigI = create_bfmatrix(numverts, numsprings); // TODO 0 springs
	id->M = create_bfmatrix(numverts, numsprings);
	id->X = create_lfvector(numverts);
	id->Xnew = create_lfvector(numverts);
	id->V = create_lfvector(numverts);
	id->Vnew = create_lfvector(numverts);
	id->F = create_lfvector(numverts);
	id->B = create_lfvector(numverts);
	id->dV = create_lfvector(numverts);
	id->z = create_lfvector(numverts);

	id->root = MEM_callocN(sizeof(RootTransform) * numverts, "root transforms");

	initdiag_bfmatrix(id->bigI, I);

	return id;
}

void BPH_mass_spring_solver_free(Implicit_Data *id)
{
	del_bfmatrix(id->A);
	del_bfmatrix(id->dFdV);
	del_bfmatrix(id->dFdX);
	del_bfmatrix(id->S);
	del_bfmatrix(id->P);
	del_bfmatrix(id->Pinv);
	del_bfmatrix(id->bigI);
	del_bfmatrix(id->M);
	
	del_lfvector(id->X);
	del_lfvector(id->Xnew);
	del_lfvector(id->V);
	del_lfvector(id->Vnew);
	del_lfvector(id->F);
	del_lfvector(id->B);
	del_lfvector(id->dV);
	del_lfvector(id->z);
	
	MEM_freeN(id->root);
	
	MEM_freeN(id);
}

/* ==== Transformation of Moving Reference Frame ====
 *   x_world, v_world, f_world, a_world, dfdx_world, dfdv_world : state variables in world space
 *   x_root, v_root, f_root, a_root, dfdx_root, dfdv_root       : state variables in root space
 *   
 *   x0 : translation of the root frame (hair root location)
 *   v0 : linear velocity of the root frame
 *   a0 : acceleration of the root frame
 *   R : rotation matrix of the root frame
 *   w : angular velocity of the root frame
 *   dwdt : angular acceleration of the root frame
 */

/* x_root = R^T * x_world */
BLI_INLINE void loc_world_to_root(float r[3], const float v[3], const RootTransform *root)
{
#ifdef CLOTH_ROOT_FRAME
	sub_v3_v3v3(r, v, root->loc);
	mul_transposed_m3_v3((float (*)[3])root->rot, r);
#else
	copy_v3_v3(r, v);
	(void)root;
#endif
}

/* x_world = R * x_root */
BLI_INLINE void loc_root_to_world(float r[3], const float v[3], const RootTransform *root)
{
#ifdef CLOTH_ROOT_FRAME
	copy_v3_v3(r, v);
	mul_m3_v3((float (*)[3])root->rot, r);
	add_v3_v3(r, root->loc);
#else
	copy_v3_v3(r, v);
	(void)root;
#endif
}

/* v_root = cross(w, x_root) + R^T*(v_world - v0) */
BLI_INLINE void vel_world_to_root(float r[3], const float x_root[3], const float v[3], const RootTransform *root)
{
#ifdef CLOTH_ROOT_FRAME
	float angvel[3];
	cross_v3_v3v3(angvel, root->omega, x_root);
	
	sub_v3_v3v3(r, v, root->vel);
	mul_transposed_m3_v3((float (*)[3])root->rot, r);
	add_v3_v3(r, angvel);
#else
	copy_v3_v3(r, v);
	(void)x_root;
	(void)root;
#endif
}

/* v_world = R*(v_root - cross(w, x_root)) + v0 */
BLI_INLINE void vel_root_to_world(float r[3], const float x_root[3], const float v[3], const RootTransform *root)
{
#ifdef CLOTH_ROOT_FRAME
	float angvel[3];
	cross_v3_v3v3(angvel, root->omega, x_root);
	
	sub_v3_v3v3(r, v, angvel);
	mul_m3_v3((float (*)[3])root->rot, r);
	add_v3_v3(r, root->vel);
#else
	copy_v3_v3(r, v);
	(void)x_root;
	(void)root;
#endif
}

/* a_root = -cross(dwdt, x_root) - 2*cross(w, v_root) - cross(w, cross(w, x_root)) + R^T*(a_world - a0) */
BLI_INLINE void force_world_to_root(float r[3], const float x_root[3], const float v_root[3], const float force[3], float mass, const RootTransform *root)
{
#ifdef CLOTH_ROOT_FRAME
	float euler[3], coriolis[3], centrifugal[3], rotvel[3];
	
	cross_v3_v3v3(euler, root->domega_dt, x_root);
	cross_v3_v3v3(coriolis, root->omega, v_root);
	mul_v3_fl(coriolis, 2.0f);
	cross_v3_v3v3(rotvel, root->omega, x_root);
	cross_v3_v3v3(centrifugal, root->omega, rotvel);
	
	madd_v3_v3v3fl(r, force, root->acc, mass);
	mul_transposed_m3_v3((float (*)[3])root->rot, r);
	madd_v3_v3fl(r, euler, mass);
	madd_v3_v3fl(r, coriolis, mass);
	madd_v3_v3fl(r, centrifugal, mass);
#else
	copy_v3_v3(r, force);
	(void)x_root;
	(void)v_root;
	(void)mass;
	(void)root;
#endif
}

/* a_world = R*[ a_root + cross(dwdt, x_root) + 2*cross(w, v_root) + cross(w, cross(w, x_root)) ] + a0 */
BLI_INLINE void force_root_to_world(float r[3], const float x_root[3], const float v_root[3], const float force[3], float mass, const RootTransform *root)
{
#ifdef CLOTH_ROOT_FRAME
	float euler[3], coriolis[3], centrifugal[3], rotvel[3];
	
	cross_v3_v3v3(euler, root->domega_dt, x_root);
	cross_v3_v3v3(coriolis, root->omega, v_root);
	mul_v3_fl(coriolis, 2.0f);
	cross_v3_v3v3(rotvel, root->omega, x_root);
	cross_v3_v3v3(centrifugal, root->omega, rotvel);
	
	madd_v3_v3v3fl(r, force, euler, mass);
	madd_v3_v3fl(r, coriolis, mass);
	madd_v3_v3fl(r, centrifugal, mass);
	mul_m3_v3((float (*)[3])root->rot, r);
	madd_v3_v3fl(r, root->acc, mass);
#else
	copy_v3_v3(r, force);
	(void)x_root;
	(void)v_root;
	(void)mass;
	(void)root;
#endif
}

BLI_INLINE void acc_world_to_root(float r[3], const float x_root[3], const float v_root[3], const float acc[3], const RootTransform *root)
{
	force_world_to_root(r, x_root, v_root, acc, 1.0f, root);
}

BLI_INLINE void acc_root_to_world(float r[3], const float x_root[3], const float v_root[3], const float acc[3], const RootTransform *root)
{
	force_root_to_world(r, x_root, v_root, acc, 1.0f, root);
}

BLI_INLINE void cross_m3_v3m3(float r[3][3], const float v[3], float m[3][3])
{
	cross_v3_v3v3(r[0], v, m[0]);
	cross_v3_v3v3(r[1], v, m[1]);
	cross_v3_v3v3(r[2], v, m[2]);
}

BLI_INLINE void cross_v3_identity(float r[3][3], const float v[3])
{
	r[0][0] = 0.0f;		r[1][0] = v[2];		r[2][0] = -v[1];
	r[0][1] = -v[2];	r[1][1] = 0.0f;		r[2][1] = v[0];
	r[0][2] = v[1];		r[1][2] = -v[0];	r[2][2] = 0.0f;
}

/* dfdx_root = m*[ -cross(dwdt, I) - cross(w, cross(w, I)) ] + R^T*(dfdx_world) */
BLI_INLINE void dfdx_world_to_root(float m[3][3], float dfdx[3][3], float mass, const RootTransform *root)
{
#ifdef CLOTH_ROOT_FRAME
	float t[3][3], u[3][3];
	
	copy_m3_m3(t, (float (*)[3])root->rot);
	transpose_m3(t);
	mul_m3_m3m3(m, t, dfdx);
	
	cross_v3_identity(t, root->domega_dt);
	mul_m3_fl(t, mass);
	sub_m3_m3m3(m, m, t);
	
	cross_v3_identity(u, root->omega);
	cross_m3_v3m3(t, root->omega, u);
	mul_m3_fl(t, mass);
	sub_m3_m3m3(m, m, t);
#else
	copy_m3_m3(m, dfdx);
	(void)mass;
	(void)root;
#endif
}

/* dfdx_world = R*(dfdx_root + m*[ cross(dwdt, I) + cross(w, cross(w, I)) ]) */
BLI_INLINE void dfdx_root_to_world(float m[3][3], float dfdx[3][3], float mass, const RootTransform *root)
{
#ifdef CLOTH_ROOT_FRAME
	float t[3][3], u[3][3];
	
	cross_v3_identity(t, root->domega_dt);
	mul_m3_fl(t, mass);
	add_m3_m3m3(m, dfdx, t);
	
	cross_v3_identity(u, root->omega);
	cross_m3_v3m3(t, root->omega, u);
	mul_m3_fl(t, mass);
	add_m3_m3m3(m, m, t);
	
	mul_m3_m3m3(m, (float (*)[3])root->rot, m);
#else
	copy_m3_m3(m, dfdx);
	(void)mass;
	(void)root;
#endif
}

/* dfdv_root = -2*m*cross(w, I) + R^T*(dfdv_world) */
BLI_INLINE void dfdv_world_to_root(float m[3][3], float dfdv[3][3], float mass, const RootTransform *root)
{
#ifdef CLOTH_ROOT_FRAME
	float t[3][3];
	
	copy_m3_m3(t, (float (*)[3])root->rot);
	transpose_m3(t);
	mul_m3_m3m3(m, t, dfdv);
	
	cross_v3_identity(t, root->omega);
	mul_m3_fl(t, 2.0f*mass);
	sub_m3_m3m3(m, m, t);
#else
	copy_m3_m3(m, dfdv);
	(void)mass;
	(void)root;
#endif
}

/* dfdv_world = R*(dfdv_root + 2*m*cross(w, I)) */
BLI_INLINE void dfdv_root_to_world(float m[3][3], float dfdv[3][3], float mass, const RootTransform *root)
{
#ifdef CLOTH_ROOT_FRAME
	float t[3][3];
	
	cross_v3_identity(t, root->omega);
	mul_m3_fl(t, 2.0f*mass);
	add_m3_m3m3(m, dfdv, t);
	
	mul_m3_m3m3(m, (float (*)[3])root->rot, m);
#else
	copy_m3_m3(m, dfdv);
	(void)mass;
	(void)root;
#endif
}

/* ================================ */

DO_INLINE float fb(float length, float L)
{
	float x = length / L;
	float xx = x * x;
	float xxx = xx * x;
	float xxxx = xxx * x;
	return (-11.541f * xxxx + 34.193f * xxx - 39.083f * xx + 23.116f * x - 9.713f);
}

DO_INLINE float fbderiv(float length, float L)
{
	float x = length/L;
	float xx = x * x;
	float xxx = xx * x;
	return (-46.164f * xxx + 102.579f * xx - 78.166f * x + 23.116f);
}

DO_INLINE float fbstar(float length, float L, float kb, float cb)
{
	float tempfb_fl = kb * fb(length, L);
	float fbstar_fl = cb * (length - L);
	
	if (tempfb_fl < fbstar_fl)
		return fbstar_fl;
	else
		return tempfb_fl;
}

// function to calculae bending spring force (taken from Choi & Co)
DO_INLINE float fbstar_jacobi(float length, float L, float kb, float cb)
{
	float tempfb_fl = kb * fb(length, L);
	float fbstar_fl = cb * (length - L);

	if (tempfb_fl < fbstar_fl) {
		return cb;
	}
	else {
		return kb * fbderiv(length, L);
	}
}

DO_INLINE void filter(lfVector *V, fmatrix3x3 *S)
{
	unsigned int i=0;

	for (i = 0; i < S[0].vcount; i++) {
		mul_m3_v3(S[i].m, V[S[i].r]);
	}
}

#if 0 /* this version of the CG algorithm does not work very well with partial constraints (where S has non-zero elements) */
static int  cg_filtered(lfVector *ldV, fmatrix3x3 *lA, lfVector *lB, lfVector *z, fmatrix3x3 *S)
{
	// Solves for unknown X in equation AX=B
	unsigned int conjgrad_loopcount=0, conjgrad_looplimit=100;
	float conjgrad_epsilon=0.0001f /* , conjgrad_lasterror=0 */ /* UNUSED */;
	lfVector *q, *d, *tmp, *r; 
	float s, starget, a, s_prev;
	unsigned int numverts = lA[0].vcount;
	q = create_lfvector(numverts);
	d = create_lfvector(numverts);
	tmp = create_lfvector(numverts);
	r = create_lfvector(numverts);

	// zero_lfvector(ldV, CLOTHPARTICLES);
	filter(ldV, S);

	add_lfvector_lfvector(ldV, ldV, z, numverts);

	// r = B - Mul(tmp, A, X);    // just use B if X known to be zero
	cp_lfvector(r, lB, numverts);
	mul_bfmatrix_lfvector(tmp, lA, ldV);
	sub_lfvector_lfvector(r, r, tmp, numverts);

	filter(r, S);

	cp_lfvector(d, r, numverts);

	s = dot_lfvector(r, r, numverts);
	starget = s * sqrtf(conjgrad_epsilon);

	while (s>starget && conjgrad_loopcount < conjgrad_looplimit) {
		// Mul(q, A, d); // q = A*d;
		mul_bfmatrix_lfvector(q, lA, d);

		filter(q, S);

		a = s/dot_lfvector(d, q, numverts);

		// X = X + d*a;
		add_lfvector_lfvectorS(ldV, ldV, d, a, numverts);

		// r = r - q*a;
		sub_lfvector_lfvectorS(r, r, q, a, numverts);

		s_prev = s;
		s = dot_lfvector(r, r, numverts);

		//d = r+d*(s/s_prev);
		add_lfvector_lfvectorS(d, r, d, (s/s_prev), numverts);

		filter(d, S);

		conjgrad_loopcount++;
	}
	/* conjgrad_lasterror = s; */ /* UNUSED */

	del_lfvector(q);
	del_lfvector(d);
	del_lfvector(tmp);
	del_lfvector(r);
	// printf("W/O conjgrad_loopcount: %d\n", conjgrad_loopcount);

	return conjgrad_loopcount<conjgrad_looplimit;  // true means we reached desired accuracy in given time - ie stable
}
#endif

static int cg_filtered(lfVector *ldV, fmatrix3x3 *lA, lfVector *lB, lfVector *z, fmatrix3x3 *S)
{
	// Solves for unknown X in equation AX=B
	unsigned int conjgrad_loopcount=0, conjgrad_looplimit=100;
	float conjgrad_epsilon=0.01f;
	
	unsigned int numverts = lA[0].vcount;
	lfVector *fB = create_lfvector(numverts);
	lfVector *AdV = create_lfvector(numverts);
	lfVector *r = create_lfvector(numverts);
	lfVector *c = create_lfvector(numverts);
	lfVector *q = create_lfvector(numverts);
	lfVector *s = create_lfvector(numverts);
	float delta_new, delta_old, delta_target, alpha;
	
	cp_lfvector(ldV, z, numverts);
	
	/* d0 = filter(B)^T * P * filter(B) */
	cp_lfvector(fB, lB, numverts);
	filter(fB, S);
	delta_target = conjgrad_epsilon*conjgrad_epsilon * dot_lfvector(fB, fB, numverts);
	
	/* r = filter(B - A * dV) */
	mul_bfmatrix_lfvector(AdV, lA, ldV);
	sub_lfvector_lfvector(r, lB, AdV, numverts);
	filter(r, S);
	
	/* c = filter(P^-1 * r) */
	cp_lfvector(c, r, numverts);
	filter(c, S);
	
	/* delta = r^T * c */
	delta_new = dot_lfvector(r, c, numverts);
	
#ifdef IMPLICIT_PRINT_SOLVER_INPUT_OUTPUT
	printf("==== A ====\n");
	print_bfmatrix(lA);
	printf("==== z ====\n");
	print_lvector(z, numverts);
	printf("==== B ====\n");
	print_lvector(lB, numverts);
	printf("==== S ====\n");
	print_bfmatrix(S);
#endif
	
	while (delta_new > delta_target && conjgrad_loopcount < conjgrad_looplimit) {
		mul_bfmatrix_lfvector(q, lA, c);
		filter(q, S);
		
		alpha = delta_new / dot_lfvector(c, q, numverts);
		
		add_lfvector_lfvectorS(ldV, ldV, c, alpha, numverts);
		
		add_lfvector_lfvectorS(r, r, q, -alpha, numverts);
		
		/* s = P^-1 * r */
		cp_lfvector(s, r, numverts);
		delta_old = delta_new;
		delta_new = dot_lfvector(r, s, numverts);
		
		add_lfvector_lfvectorS(c, s, c, delta_new / delta_old, numverts);
		filter(c, S);
		
		conjgrad_loopcount++;
	}

#ifdef IMPLICIT_PRINT_SOLVER_INPUT_OUTPUT
	printf("==== dV ====\n");
	print_lvector(ldV, numverts);
	printf("========\n");
#endif
	
	del_lfvector(fB);
	del_lfvector(AdV);
	del_lfvector(r);
	del_lfvector(c);
	del_lfvector(q);
	del_lfvector(s);
	// printf("W/O conjgrad_loopcount: %d\n", conjgrad_loopcount);

	return conjgrad_loopcount < conjgrad_looplimit;  // true means we reached desired accuracy in given time - ie stable
}

#if 0
// block diagonalizer
DO_INLINE void BuildPPinv(fmatrix3x3 *lA, fmatrix3x3 *P, fmatrix3x3 *Pinv)
{
	unsigned int i = 0;
	
	// Take only the diagonal blocks of A
// #pragma omp parallel for private(i) if (lA[0].vcount > CLOTH_OPENMP_LIMIT)
	for (i = 0; i<lA[0].vcount; i++) {
		// block diagonalizer
		cp_fmatrix(P[i].m, lA[i].m);
		inverse_fmatrix(Pinv[i].m, P[i].m);
		
	}
}
/*
// version 1.3
static int cg_filtered_pre(lfVector *dv, fmatrix3x3 *lA, lfVector *lB, lfVector *z, fmatrix3x3 *S, fmatrix3x3 *P, fmatrix3x3 *Pinv)
{
	unsigned int numverts = lA[0].vcount, iterations = 0, conjgrad_looplimit=100;
	float delta0 = 0, deltaNew = 0, deltaOld = 0, alpha = 0;
	float conjgrad_epsilon=0.0001; // 0.2 is dt for steps=5
	lfVector *r = create_lfvector(numverts);
	lfVector *p = create_lfvector(numverts);
	lfVector *s = create_lfvector(numverts);
	lfVector *h = create_lfvector(numverts);
	
	BuildPPinv(lA, P, Pinv);
	
	filter(dv, S);
	add_lfvector_lfvector(dv, dv, z, numverts);
	
	mul_bfmatrix_lfvector(r, lA, dv);
	sub_lfvector_lfvector(r, lB, r, numverts);
	filter(r, S);
	
	mul_prevfmatrix_lfvector(p, Pinv, r);
	filter(p, S);
	
	deltaNew = dot_lfvector(r, p, numverts);
	
	delta0 = deltaNew * sqrt(conjgrad_epsilon);
	
	// itstart();
	
	while ((deltaNew > delta0) && (iterations < conjgrad_looplimit))
	{
		iterations++;
		
		mul_bfmatrix_lfvector(s, lA, p);
		filter(s, S);
		
		alpha = deltaNew / dot_lfvector(p, s, numverts);
		
		add_lfvector_lfvectorS(dv, dv, p, alpha, numverts);
		
		add_lfvector_lfvectorS(r, r, s, -alpha, numverts);
		
		mul_prevfmatrix_lfvector(h, Pinv, r);
		filter(h, S);
		
		deltaOld = deltaNew;
		
		deltaNew = dot_lfvector(r, h, numverts);
		
		add_lfvector_lfvectorS(p, h, p, deltaNew / deltaOld, numverts);
		
		filter(p, S);
		
	}
	
	// itend();
	// printf("cg_filtered_pre time: %f\n", (float)itval());
	
	del_lfvector(h);
	del_lfvector(s);
	del_lfvector(p);
	del_lfvector(r);
	
	printf("iterations: %d\n", iterations);
	
	return iterations<conjgrad_looplimit;
}
*/
// version 1.4
static int cg_filtered_pre(lfVector *dv, fmatrix3x3 *lA, lfVector *lB, lfVector *z, fmatrix3x3 *S, fmatrix3x3 *P, fmatrix3x3 *Pinv, fmatrix3x3 *bigI)
{
	unsigned int numverts = lA[0].vcount, iterations = 0, conjgrad_looplimit=100;
	float delta0 = 0, deltaNew = 0, deltaOld = 0, alpha = 0, tol = 0;
	lfVector *r = create_lfvector(numverts);
	lfVector *p = create_lfvector(numverts);
	lfVector *s = create_lfvector(numverts);
	lfVector *h = create_lfvector(numverts);
	lfVector *bhat = create_lfvector(numverts);
	lfVector *btemp = create_lfvector(numverts);
	
	BuildPPinv(lA, P, Pinv);
	
	initdiag_bfmatrix(bigI, I);
	sub_bfmatrix_Smatrix(bigI, bigI, S);
	
	// x = Sx_0+(I-S)z
	filter(dv, S);
	add_lfvector_lfvector(dv, dv, z, numverts);
	
	// b_hat = S(b-A(I-S)z)
	mul_bfmatrix_lfvector(r, lA, z);
	mul_bfmatrix_lfvector(bhat, bigI, r);
	sub_lfvector_lfvector(bhat, lB, bhat, numverts);
	
	// r = S(b-Ax)
	mul_bfmatrix_lfvector(r, lA, dv);
	sub_lfvector_lfvector(r, lB, r, numverts);
	filter(r, S);
	
	// p = SP^-1r
	mul_prevfmatrix_lfvector(p, Pinv, r);
	filter(p, S);
	
	// delta0 = bhat^TP^-1bhat
	mul_prevfmatrix_lfvector(btemp, Pinv, bhat);
	delta0 = dot_lfvector(bhat, btemp, numverts);
	
	// deltaNew = r^TP
	deltaNew = dot_lfvector(r, p, numverts);
	
	/*
	filter(dv, S);
	add_lfvector_lfvector(dv, dv, z, numverts);
	
	mul_bfmatrix_lfvector(r, lA, dv);
	sub_lfvector_lfvector(r, lB, r, numverts);
	filter(r, S);
	
	mul_prevfmatrix_lfvector(p, Pinv, r);
	filter(p, S);
	
	deltaNew = dot_lfvector(r, p, numverts);
	
	delta0 = deltaNew * sqrt(conjgrad_epsilon);
	*/
	
	// itstart();
	
	tol = (0.01*0.2);
	
	while ((deltaNew > delta0*tol*tol) && (iterations < conjgrad_looplimit))
	{
		iterations++;
		
		mul_bfmatrix_lfvector(s, lA, p);
		filter(s, S);
		
		alpha = deltaNew / dot_lfvector(p, s, numverts);
		
		add_lfvector_lfvectorS(dv, dv, p, alpha, numverts);
		
		add_lfvector_lfvectorS(r, r, s, -alpha, numverts);
		
		mul_prevfmatrix_lfvector(h, Pinv, r);
		filter(h, S);
		
		deltaOld = deltaNew;
		
		deltaNew = dot_lfvector(r, h, numverts);
		
		add_lfvector_lfvectorS(p, h, p, deltaNew / deltaOld, numverts);
		
		filter(p, S);
		
	}
	
	// itend();
	// printf("cg_filtered_pre time: %f\n", (float)itval());
	
	del_lfvector(btemp);
	del_lfvector(bhat);
	del_lfvector(h);
	del_lfvector(s);
	del_lfvector(p);
	del_lfvector(r);
	
	// printf("iterations: %d\n", iterations);
	
	return iterations<conjgrad_looplimit;
}
#endif

DO_INLINE void dfdx_spring_type2(float to[3][3], float dir[3], float length, float L, float k, float cb)
{
	// return  outerprod(dir, dir)*fbstar_jacobi(length, L, k, cb);
	mul_fvectorT_fvectorS(to, dir, dir, fbstar_jacobi(length, L, k, cb));
}

DO_INLINE void dfdv_damp(float to[3][3], float dir[3], float damping)
{
	// derivative of force wrt velocity.  
	mul_fvectorT_fvectorS(to, dir, dir, damping);
	
}

DO_INLINE void dfdx_spring(float to[3][3],  float dir[3], float length, float L, float k)
{
	// dir is unit length direction, rest is spring's restlength, k is spring constant.
	//return  ( (I-outerprod(dir, dir))*Min(1.0f, rest/length) - I) * -k;
	mul_fvectorT_fvector(to, dir, dir);
	sub_fmatrix_fmatrix(to, I, to);

	mul_fmatrix_S(to, (L/length)); 
	sub_fmatrix_fmatrix(to, to, I);
	mul_fmatrix_S(to, -k);
}

DO_INLINE void cloth_calc_spring_force(ClothModifierData *clmd, ClothSpring *s, lfVector *UNUSED(lF), lfVector *X, lfVector *V, fmatrix3x3 *UNUSED(dFdV), fmatrix3x3 *UNUSED(dFdX), float time)
{
	Cloth *cloth = clmd->clothObject;
	ClothVertex *verts = cloth->verts;
	float extent[3];
	float length = 0, dot = 0;
	float dir[3] = {0, 0, 0};
	float vel[3];
	float k = 0.0f;
	float L = s->restlen;
	float cb; /* = clmd->sim_parms->structural; */ /*UNUSED*/

	float nullf[3] = {0, 0, 0};
	float stretch_force[3] = {0, 0, 0};
	float bending_force[3] = {0, 0, 0};
	float damping_force[3] = {0, 0, 0};
	float nulldfdx[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
	
	float scaling = 0.0;

	int no_compress = clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_NO_SPRING_COMPRESS;
	
	copy_v3_v3(s->f, nullf);
	cp_fmatrix(s->dfdx, nulldfdx);
	cp_fmatrix(s->dfdv, nulldfdx);

	// calculate elonglation
	sub_v3_v3v3(extent, X[s->kl], X[s->ij]);
	sub_v3_v3v3(vel, V[s->kl], V[s->ij]);
	dot = dot_v3v3(extent, extent);
	length = sqrtf(dot);
	
	s->flags &= ~CLOTH_SPRING_FLAG_NEEDED;
	
	if (length > ALMOST_ZERO) {
		/*
		if (length>L)
		{
		if ((clmd->sim_parms->flags & CSIMSETT_FLAG_TEARING_ENABLED) &&
		    ((((length-L)*100.0f/L) > clmd->sim_parms->maxspringlen))) // cut spring!
		{
		s->flags |= CSPRING_FLAG_DEACTIVATE;
		return;
	}
	}
		*/
		mul_fvector_S(dir, extent, 1.0f/length);
	}
	else {
		mul_fvector_S(dir, extent, 0.0f);
	}
	
	// calculate force of structural + shear springs
	if ((s->type & CLOTH_SPRING_TYPE_STRUCTURAL) || (s->type & CLOTH_SPRING_TYPE_SHEAR) || (s->type & CLOTH_SPRING_TYPE_SEWING) ) {
#ifdef CLOTH_FORCE_SPRING_STRUCTURAL
		if (length > L || no_compress) {
			s->flags |= CLOTH_SPRING_FLAG_NEEDED;
			
			k = clmd->sim_parms->structural;

			scaling = k + s->stiffness * fabsf(clmd->sim_parms->max_struct - k);

			k = scaling / (clmd->sim_parms->avg_spring_len + FLT_EPSILON);

			// TODO: verify, half verified (couldn't see error)
			if (s->type & CLOTH_SPRING_TYPE_SEWING) {
				// sewing springs usually have a large distance at first so clamp the force so we don't get tunnelling through colission objects
				float force = k*(length-L);
				if (force > clmd->sim_parms->max_sewing) {
					force = clmd->sim_parms->max_sewing;
				}
				mul_fvector_S(stretch_force, dir, force);
			}
			else {
				mul_fvector_S(stretch_force, dir, k * (length - L));
			}

			VECADD(s->f, s->f, stretch_force);

			// Ascher & Boxman, p.21: Damping only during elonglation
			// something wrong with it...
			mul_fvector_S(damping_force, dir, clmd->sim_parms->Cdis * dot_v3v3(vel, dir));
			VECADD(s->f, s->f, damping_force);
			
			/* VERIFIED */
			dfdx_spring(s->dfdx, dir, length, L, k);
			
			/* VERIFIED */
			dfdv_damp(s->dfdv, dir, clmd->sim_parms->Cdis);
			
		}
#endif
	}
	else if (s->type & CLOTH_SPRING_TYPE_GOAL) {
#ifdef CLOTH_FORCE_SPRING_GOAL
		float tvect[3];
		
		s->flags |= CLOTH_SPRING_FLAG_NEEDED;
		
		// current_position = xold + t * (newposition - xold)
		sub_v3_v3v3(tvect, verts[s->ij].xconst, verts[s->ij].xold);
		mul_fvector_S(tvect, tvect, time);
		VECADD(tvect, tvect, verts[s->ij].xold);

		sub_v3_v3v3(extent, X[s->ij], tvect);
		
		// SEE MSG BELOW (these are UNUSED)
		// dot = dot_v3v3(extent, extent);
		// length = sqrt(dot);
		
		k = clmd->sim_parms->goalspring;
		
		scaling = k + s->stiffness * fabsf(clmd->sim_parms->max_struct - k);
			
		k = verts [s->ij].goal * scaling / (clmd->sim_parms->avg_spring_len + FLT_EPSILON);
		
		VECADDS(s->f, s->f, extent, -k);
		
		mul_fvector_S(damping_force, dir, clmd->sim_parms->goalfrict * 0.01f * dot_v3v3(vel, dir));
		VECADD(s->f, s->f, damping_force);
		
		// HERE IS THE PROBLEM!!!!
		// dfdx_spring(s->dfdx, dir, length, 0.0, k);
		// dfdv_damp(s->dfdv, dir, MIN2(1.0, (clmd->sim_parms->goalfrict/100.0)));
#endif
	}
	else {  /* calculate force of bending springs */
#ifdef CLOTH_FORCE_SPRING_BEND
		if (length < L) {
			s->flags |= CLOTH_SPRING_FLAG_NEEDED;
			
			k = clmd->sim_parms->bending;
			
			scaling = k + s->stiffness * fabsf(clmd->sim_parms->max_bend - k);
			cb = k = scaling / (20.0f * (clmd->sim_parms->avg_spring_len + FLT_EPSILON));

			mul_fvector_S(bending_force, dir, fbstar(length, L, k, cb));
			VECADD(s->f, s->f, bending_force);

			dfdx_spring_type2(s->dfdx, dir, length, L, k, cb);
		}
#endif
	}
}

DO_INLINE void cloth_apply_spring_force(ClothModifierData *UNUSED(clmd), ClothSpring *s, lfVector *lF, lfVector *UNUSED(X), lfVector *UNUSED(V), fmatrix3x3 *dFdV, fmatrix3x3 *dFdX)
{
	if (s->flags & CLOTH_SPRING_FLAG_NEEDED) {
		if (!(s->type & CLOTH_SPRING_TYPE_BENDING)) {
			sub_fmatrix_fmatrix(dFdV[s->ij].m, dFdV[s->ij].m, s->dfdv);
			sub_fmatrix_fmatrix(dFdV[s->kl].m, dFdV[s->kl].m, s->dfdv);
			add_fmatrix_fmatrix(dFdV[s->matrix_index].m, dFdV[s->matrix_index].m, s->dfdv);
		}

		VECADD(lF[s->ij], lF[s->ij], s->f);
		
		if (!(s->type & CLOTH_SPRING_TYPE_GOAL))
			sub_v3_v3v3(lF[s->kl], lF[s->kl], s->f);
		
		sub_fmatrix_fmatrix(dFdX[s->kl].m, dFdX[s->kl].m, s->dfdx);
		sub_fmatrix_fmatrix(dFdX[s->ij].m, dFdX[s->ij].m, s->dfdx);
		add_fmatrix_fmatrix(dFdX[s->matrix_index].m, dFdX[s->matrix_index].m, s->dfdx);
	}
}


static void CalcFloat( float *v1, float *v2, float *v3, float *n)
{
	float n1[3], n2[3];

	n1[0] = v1[0]-v2[0];
	n2[0] = v2[0]-v3[0];
	n1[1] = v1[1]-v2[1];
	n2[1] = v2[1]-v3[1];
	n1[2] = v1[2]-v2[2];
	n2[2] = v2[2]-v3[2];
	n[0] = n1[1]*n2[2]-n1[2]*n2[1];
	n[1] = n1[2]*n2[0]-n1[0]*n2[2];
	n[2] = n1[0]*n2[1]-n1[1]*n2[0];
}

static void CalcFloat4( float *v1, float *v2, float *v3, float *v4, float *n)
{
	/* real cross! */
	float n1[3], n2[3];

	n1[0] = v1[0]-v3[0];
	n1[1] = v1[1]-v3[1];
	n1[2] = v1[2]-v3[2];

	n2[0] = v2[0]-v4[0];
	n2[1] = v2[1]-v4[1];
	n2[2] = v2[2]-v4[2];

	n[0] = n1[1]*n2[2]-n1[2]*n2[1];
	n[1] = n1[2]*n2[0]-n1[0]*n2[2];
	n[2] = n1[0]*n2[1]-n1[1]*n2[0];
}

static float calculateVertexWindForce(const float wind[3], const float vertexnormal[3])
{
	return dot_v3v3(wind, vertexnormal);
}

/* ================ Volumetric Hair Interaction ================
 * adapted from
 *      Volumetric Methods for Simulation and Rendering of Hair
 *      by Lena Petrovic, Mark Henne and John Anderson
 *      Pixar Technical Memo #06-08, Pixar Animation Studios
 */

/* Note about array indexing:
 * Generally the arrays here are one-dimensional.
 * The relation between 3D indices and the array offset is
 *   offset = x + res_x * y + res_y * z
 */

/* TODO: This is an initial implementation and should be made much better in due time.
 * What should at least be implemented is a grid size parameter and a smoothing kernel
 * for bigger grids.
 */

/* 10x10x10 grid gives nice initial results */
static const int hair_grid_res = 10;

static int hair_grid_size(int res)
{
	return res * res * res;
}

BLI_INLINE void hair_grid_get_scale(int res, const float gmin[3], const float gmax[3], float scale[3])
{
	sub_v3_v3v3(scale, gmax, gmin);
	mul_v3_fl(scale, 1.0f / (res-1));
}

typedef struct HairGridVert {
	float velocity[3];
	float density;
} HairGridVert;

#define HAIR_GRID_INDEX_AXIS(vec, res, gmin, scale, axis) ( min_ii( max_ii( (int)((vec[axis] - gmin[axis]) / scale[axis]), 0), res-2 ) )

BLI_INLINE int hair_grid_offset(const float vec[3], int res, const float gmin[3], const float scale[3])
{
	int i, j, k;
	i = HAIR_GRID_INDEX_AXIS(vec, res, gmin, scale, 0);
	j = HAIR_GRID_INDEX_AXIS(vec, res, gmin, scale, 1);
	k = HAIR_GRID_INDEX_AXIS(vec, res, gmin, scale, 2);
	return i + (j + k*res)*res;
}

BLI_INLINE int hair_grid_interp_weights(int res, const float gmin[3], const float scale[3], const float vec[3], float uvw[3])
{
	int i, j, k, offset;
	
	i = HAIR_GRID_INDEX_AXIS(vec, res, gmin, scale, 0);
	j = HAIR_GRID_INDEX_AXIS(vec, res, gmin, scale, 1);
	k = HAIR_GRID_INDEX_AXIS(vec, res, gmin, scale, 2);
	offset = i + (j + k*res)*res;
	
	uvw[0] = (vec[0] - gmin[0]) / scale[0] - (float)i;
	uvw[1] = (vec[1] - gmin[1]) / scale[1] - (float)j;
	uvw[2] = (vec[2] - gmin[2]) / scale[2] - (float)k;
	
	return offset;
}

BLI_INLINE void hair_grid_interpolate(const HairGridVert *grid, int res, const float gmin[3], const float scale[3], const float vec[3],
                                      float *density, float velocity[3], float density_gradient[3])
{
	HairGridVert data[8];
	float uvw[3], muvw[3];
	int res2 = res * res;
	int offset;
	
	offset = hair_grid_interp_weights(res, gmin, scale, vec, uvw);
	muvw[0] = 1.0f - uvw[0];
	muvw[1] = 1.0f - uvw[1];
	muvw[2] = 1.0f - uvw[2];
	
	data[0] = grid[offset           ];
	data[1] = grid[offset         +1];
	data[2] = grid[offset     +res  ];
	data[3] = grid[offset     +res+1];
	data[4] = grid[offset+res2      ];
	data[5] = grid[offset+res2    +1];
	data[6] = grid[offset+res2+res  ];
	data[7] = grid[offset+res2+res+1];
	
	if (density) {
		*density = muvw[2]*( muvw[1]*( muvw[0]*data[0].density + uvw[0]*data[1].density )   +
		                      uvw[1]*( muvw[0]*data[2].density + uvw[0]*data[3].density ) ) +
		            uvw[2]*( muvw[1]*( muvw[0]*data[4].density + uvw[0]*data[5].density )   +
		                      uvw[1]*( muvw[0]*data[6].density + uvw[0]*data[7].density ) );
	}
	if (velocity) {
		int k;
		for (k = 0; k < 3; ++k) {
			velocity[k] = muvw[2]*( muvw[1]*( muvw[0]*data[0].velocity[k] + uvw[0]*data[1].velocity[k] )   +
			                         uvw[1]*( muvw[0]*data[2].velocity[k] + uvw[0]*data[3].velocity[k] ) ) +
			               uvw[2]*( muvw[1]*( muvw[0]*data[4].velocity[k] + uvw[0]*data[5].velocity[k] )   +
			                         uvw[1]*( muvw[0]*data[6].velocity[k] + uvw[0]*data[7].velocity[k] ) );
		}
	}
	if (density_gradient) {
		density_gradient[0] = muvw[1] * muvw[2] * ( data[0].density - data[1].density ) +
		                       uvw[1] * muvw[2] * ( data[2].density - data[3].density ) +
		                      muvw[1] *  uvw[2] * ( data[4].density - data[5].density ) +
		                       uvw[1] *  uvw[2] * ( data[6].density - data[7].density );
		
		density_gradient[1] = muvw[2] * muvw[0] * ( data[0].density - data[2].density ) +
		                       uvw[2] * muvw[0] * ( data[4].density - data[6].density ) +
		                      muvw[2] *  uvw[0] * ( data[1].density - data[3].density ) +
		                       uvw[2] *  uvw[0] * ( data[5].density - data[7].density );
		
		density_gradient[2] = muvw[2] * muvw[0] * ( data[0].density - data[4].density ) +
		                       uvw[2] * muvw[0] * ( data[1].density - data[5].density ) +
		                      muvw[2] *  uvw[0] * ( data[2].density - data[6].density ) +
		                       uvw[2] *  uvw[0] * ( data[3].density - data[7].density );
	}
}

static void hair_velocity_smoothing(const HairGridVert *hairgrid, const float gmin[3], const float scale[3], float smoothfac,
                                    lfVector *lF, lfVector *lX, lfVector *lV, unsigned int numverts)
{
	int v;
	/* calculate forces */
	for (v = 0; v < numverts; v++) {
		float density, velocity[3];
		
		hair_grid_interpolate(hairgrid, hair_grid_res, gmin, scale, lX[v], &density, velocity, NULL);
		
		sub_v3_v3(velocity, lV[v]);
		madd_v3_v3fl(lF[v], velocity, smoothfac);
	}
}

static void hair_velocity_collision(const HairGridVert *collgrid, const float gmin[3], const float scale[3], float collfac,
                                    lfVector *lF, lfVector *lX, lfVector *lV, unsigned int numverts)
{
	int v;
	/* calculate forces */
	for (v = 0; v < numverts; v++) {
		int offset = hair_grid_offset(lX[v], hair_grid_res, gmin, scale);
		
		if (collgrid[offset].density > 0.0f) {
			lF[v][0] += collfac * (collgrid[offset].velocity[0] - lV[v][0]);
			lF[v][1] += collfac * (collgrid[offset].velocity[1] - lV[v][1]);
			lF[v][2] += collfac * (collgrid[offset].velocity[2] - lV[v][2]);
		}
	}
}

static void hair_pressure_force(const HairGridVert *hairgrid, const float gmin[3], const float scale[3], float pressurefac, float minpressure,
                                lfVector *lF, lfVector *lX, unsigned int numverts)
{
	int v;
	
	/* calculate forces */
	for (v = 0; v < numverts; v++) {
		float density, gradient[3], gradlen;
		
		hair_grid_interpolate(hairgrid, hair_grid_res, gmin, scale, lX[v], &density, NULL, gradient);
		
		gradlen = normalize_v3(gradient) - minpressure;
		if (gradlen < 0.0f)
			continue;
		mul_v3_fl(gradient, gradlen);
		
		madd_v3_v3fl(lF[v], gradient, pressurefac);
	}
}

static void hair_volume_get_boundbox(lfVector *lX, unsigned int numverts, float gmin[3], float gmax[3])
{
	int i;
	
	INIT_MINMAX(gmin, gmax);
	for (i = 0; i < numverts; i++)
		DO_MINMAX(lX[i], gmin, gmax);
}

BLI_INLINE bool hair_grid_point_valid(const float vec[3], float gmin[3], float gmax[3])
{
	return !(vec[0] < gmin[0] || vec[1] < gmin[1] || vec[2] < gmin[2] ||
	         vec[0] > gmax[0] || vec[1] > gmax[1] || vec[2] > gmax[2]);
}

BLI_INLINE float dist_tent_v3f3(const float a[3], float x, float y, float z)
{
	float w = (1.0f - fabsf(a[0] - x)) * (1.0f - fabsf(a[1] - y)) * (1.0f - fabsf(a[2] - z));
	return w;
}

/* returns the grid array offset as well to avoid redundant calculation */
static int hair_grid_weights(int res, const float gmin[3], const float scale[3], const float vec[3], float weights[8])
{
	int i, j, k, offset;
	float uvw[3];
	
	i = HAIR_GRID_INDEX_AXIS(vec, res, gmin, scale, 0);
	j = HAIR_GRID_INDEX_AXIS(vec, res, gmin, scale, 1);
	k = HAIR_GRID_INDEX_AXIS(vec, res, gmin, scale, 2);
	offset = i + (j + k*res)*res;
	
	uvw[0] = (vec[0] - gmin[0]) / scale[0];
	uvw[1] = (vec[1] - gmin[1]) / scale[1];
	uvw[2] = (vec[2] - gmin[2]) / scale[2];
	
	weights[0] = dist_tent_v3f3(uvw, (float)i    , (float)j    , (float)k    );
	weights[1] = dist_tent_v3f3(uvw, (float)(i+1), (float)j    , (float)k    );
	weights[2] = dist_tent_v3f3(uvw, (float)i    , (float)(j+1), (float)k    );
	weights[3] = dist_tent_v3f3(uvw, (float)(i+1), (float)(j+1), (float)k    );
	weights[4] = dist_tent_v3f3(uvw, (float)i    , (float)j    , (float)(k+1));
	weights[5] = dist_tent_v3f3(uvw, (float)(i+1), (float)j    , (float)(k+1));
	weights[6] = dist_tent_v3f3(uvw, (float)i    , (float)(j+1), (float)(k+1));
	weights[7] = dist_tent_v3f3(uvw, (float)(i+1), (float)(j+1), (float)(k+1));
	
	return offset;
}

static HairGridVert *hair_volume_create_hair_grid(ClothModifierData *clmd, lfVector *lX, lfVector *lV, unsigned int numverts)
{
	int res = hair_grid_res;
	int size = hair_grid_size(res);
	HairGridVert *hairgrid;
	float gmin[3], gmax[3], scale[3];
	/* 2.0f is an experimental value that seems to give good results */
	float smoothfac = 2.0f * clmd->sim_parms->velocity_smooth;
	unsigned int	v = 0;
	int	            i = 0;

	hair_volume_get_boundbox(lX, numverts, gmin, gmax);
	hair_grid_get_scale(res, gmin, gmax, scale);

	hairgrid = MEM_mallocN(sizeof(HairGridVert) * size, "hair voxel data");

	/* initialize grid */
	for (i = 0; i < size; ++i) {
		zero_v3(hairgrid[i].velocity);
		hairgrid[i].density = 0.0f;
	}

	/* gather velocities & density */
	if (smoothfac > 0.0f) {
		for (v = 0; v < numverts; v++) {
			float *V = lV[v];
			float weights[8];
			int di, dj, dk;
			int offset;
			
			if (!hair_grid_point_valid(lX[v], gmin, gmax))
				continue;
			
			offset = hair_grid_weights(res, gmin, scale, lX[v], weights);
			
			for (di = 0; di < 2; ++di) {
				for (dj = 0; dj < 2; ++dj) {
					for (dk = 0; dk < 2; ++dk) {
						int voffset = offset + di + (dj + dk*res)*res;
						int iw = di + dj*2 + dk*4;
						
						hairgrid[voffset].density += weights[iw];
						madd_v3_v3fl(hairgrid[voffset].velocity, V, weights[iw]);
					}
				}
			}
		}
	}

	/* divide velocity with density */
	for (i = 0; i < size; i++) {
		float density = hairgrid[i].density;
		if (density > 0.0f)
			mul_v3_fl(hairgrid[i].velocity, 1.0f/density);
	}
	
	return hairgrid;
}


static HairGridVert *hair_volume_create_collision_grid(ClothModifierData *clmd, lfVector *lX, unsigned int numverts)
{
	int res = hair_grid_res;
	int size = hair_grid_size(res);
	HairGridVert *collgrid;
	ListBase *colliders;
	ColliderCache *col = NULL;
	float gmin[3], gmax[3], scale[3];
	/* 2.0f is an experimental value that seems to give good results */
	float collfac = 2.0f * clmd->sim_parms->collider_friction;
	unsigned int	v = 0;
	int	            i = 0;

	hair_volume_get_boundbox(lX, numverts, gmin, gmax);
	hair_grid_get_scale(res, gmin, gmax, scale);

	collgrid = MEM_mallocN(sizeof(HairGridVert) * size, "hair collider voxel data");

	/* initialize grid */
	for (i = 0; i < size; ++i) {
		zero_v3(collgrid[i].velocity);
		collgrid[i].density = 0.0f;
	}

	/* gather colliders */
	colliders = get_collider_cache(clmd->scene, NULL, NULL);
	if (colliders && collfac > 0.0f) {
		for (col = colliders->first; col; col = col->next) {
			MVert *loc0 = col->collmd->x;
			MVert *loc1 = col->collmd->xnew;
			float vel[3];
			float weights[8];
			int di, dj, dk;
			
			for (v=0; v < col->collmd->numverts; v++, loc0++, loc1++) {
				int offset;
				
				if (!hair_grid_point_valid(loc1->co, gmin, gmax))
					continue;
				
				offset = hair_grid_weights(res, gmin, scale, lX[v], weights);
				
				sub_v3_v3v3(vel, loc1->co, loc0->co);
				
				for (di = 0; di < 2; ++di) {
					for (dj = 0; dj < 2; ++dj) {
						for (dk = 0; dk < 2; ++dk) {
							int voffset = offset + di + (dj + dk*res)*res;
							int iw = di + dj*2 + dk*4;
							
							collgrid[voffset].density += weights[iw];
							madd_v3_v3fl(collgrid[voffset].velocity, vel, weights[iw]);
						}
					}
				}
			}
		}
	}
	free_collider_cache(&colliders);

	/* divide velocity with density */
	for (i = 0; i < size; i++) {
		float density = collgrid[i].density;
		if (density > 0.0f)
			mul_v3_fl(collgrid[i].velocity, 1.0f/density);
	}
	
	return collgrid;
}

static void hair_volume_forces(ClothModifierData *clmd, lfVector *lF, lfVector *lX, lfVector *lV, unsigned int numverts)
{
	HairGridVert *hairgrid, *collgrid;
	float gmin[3], gmax[3], scale[3];
	/* 2.0f is an experimental value that seems to give good results */
	float smoothfac = 2.0f * clmd->sim_parms->velocity_smooth;
	float collfac = 2.0f * clmd->sim_parms->collider_friction;
	float pressfac = clmd->sim_parms->pressure;
	float minpress = clmd->sim_parms->pressure_threshold;
	
	if (smoothfac <= 0.0f && collfac <= 0.0f && pressfac <= 0.0f)
		return;
	
	hair_volume_get_boundbox(lX, numverts, gmin, gmax);
	hair_grid_get_scale(hair_grid_res, gmin, gmax, scale);
	
	hairgrid = hair_volume_create_hair_grid(clmd, lX, lV, numverts);
	collgrid = hair_volume_create_collision_grid(clmd, lX, numverts);
	
	hair_velocity_smoothing(hairgrid, gmin, scale, smoothfac, lF, lX, lV, numverts);
	hair_velocity_collision(collgrid, gmin, scale, collfac, lF, lX, lV, numverts);
	hair_pressure_force(hairgrid, gmin, scale, pressfac, minpress, lF, lX, numverts);
	
	MEM_freeN(hairgrid);
	MEM_freeN(collgrid);
}

bool implicit_hair_volume_get_texture_data(Object *UNUSED(ob), ClothModifierData *clmd, ListBase *UNUSED(effectors), VoxelData *vd)
{
	lfVector *lX, *lV;
	HairGridVert *hairgrid/*, *collgrid*/;
	int numverts;
	int totres, i;
	int depth;

	if (!clmd->clothObject || !clmd->clothObject->implicit)
		return false;

	lX = clmd->clothObject->implicit->X;
	lV = clmd->clothObject->implicit->V;
	numverts = clmd->clothObject->numverts;

	hairgrid = hair_volume_create_hair_grid(clmd, lX, lV, numverts);
//	collgrid = hair_volume_create_collision_grid(clmd, lX, numverts);

	vd->resol[0] = hair_grid_res;
	vd->resol[1] = hair_grid_res;
	vd->resol[2] = hair_grid_res;
	
	totres = hair_grid_size(hair_grid_res);
	
	if (vd->hair_type == TEX_VD_HAIRVELOCITY) {
		depth = 4;
		vd->data_type = TEX_VD_RGBA_PREMUL;
	}
	else {
		depth = 1;
		vd->data_type = TEX_VD_INTENSITY;
	}
	
	if (totres > 0) {
		vd->dataset = (float *)MEM_mapallocN(sizeof(float) * depth * (totres), "hair volume texture data");
		
		for (i = 0; i < totres; ++i) {
			switch (vd->hair_type) {
				case TEX_VD_HAIRDENSITY:
					vd->dataset[i] = hairgrid[i].density;
					break;
				
				case TEX_VD_HAIRRESTDENSITY:
					vd->dataset[i] = 0.0f; // TODO
					break;
				
				case TEX_VD_HAIRVELOCITY:
					vd->dataset[i + 0*totres] = hairgrid[i].velocity[0];
					vd->dataset[i + 1*totres] = hairgrid[i].velocity[1];
					vd->dataset[i + 2*totres] = hairgrid[i].velocity[2];
					vd->dataset[i + 3*totres] = len_v3(hairgrid[i].velocity);
					break;
				
				case TEX_VD_HAIRENERGY:
					vd->dataset[i] = 0.0f; // TODO
					break;
			}
		}
	}
	else {
		vd->dataset = NULL;
	}
	
	MEM_freeN(hairgrid);
//	MEM_freeN(collgrid);
	
	return true;
}

/* ================================ */

static void cloth_calc_force(ClothModifierData *clmd, float UNUSED(frame), lfVector *lF, lfVector *lX, lfVector *lV, fmatrix3x3 *dFdV, fmatrix3x3 *dFdX, ListBase *effectors, float time, fmatrix3x3 *M)
{
	/* Collect forces and derivatives:  F, dFdX, dFdV */
	Cloth 		*cloth 		= clmd->clothObject;
	ClothVertex *verts = cloth->verts;
	RootTransform *roots = cloth->implicit->root;
	unsigned int i	= 0;
	float 		drag 	= clmd->sim_parms->Cvi * 0.01f; /* viscosity of air scaled in percent */
	float 		gravity[3] = {0.0f, 0.0f, 0.0f};
	MFace 		*mfaces 	= cloth->mfaces;
	unsigned int numverts = cloth->numverts;
	LinkNode *search;
	lfVector *winvec;
	EffectedPoint epoint;
	
	/* initialize forces to zero */
	zero_lfvector(lF, numverts);
	init_bfmatrix(dFdX, ZERO);
	init_bfmatrix(dFdV, ZERO);

#ifdef CLOTH_FORCE_GRAVITY
	/* global acceleration (gravitation) */
	if (clmd->scene->physics_settings.flag & PHYS_GLOBAL_GRAVITY) {
		copy_v3_v3(gravity, clmd->scene->physics_settings.gravity);
		mul_fvector_S(gravity, gravity, 0.001f * clmd->sim_parms->effector_weights->global_gravity); /* scale gravity force */
	}
	/* multiply lF with mass matrix
	 * force = mass * acceleration (in this case: gravity)
	 */
	for (i = 0; i < numverts; i++) {
		float g[3];
		acc_world_to_root(g, lX[i], lV[i], gravity, &roots[i]);
		mul_m3_v3(M[i].m, g);
		add_v3_v3(lF[i], g);
	}
#else
	zero_lfvector(lF, numverts);
#endif

	hair_volume_forces(clmd, lF, lX, lV, numverts);

#ifdef CLOTH_FORCE_DRAG
	/* set dFdX jacobi matrix diagonal entries to -spring_air */ 
	for (i = 0; i < numverts; i++) {
		dFdV[i].m[0][0] -= drag;
		dFdV[i].m[1][1] -= drag;
		dFdV[i].m[2][2] -= drag;
	}
	submul_lfvectorS(lF, lV, drag, numverts);
	for (i = 0; i < numverts; i++) {
#if 1
		float tmp[3][3];
		
		/* NB: uses root space velocity, no need to transform */
		madd_v3_v3fl(lF[i], lV[i], -drag);
		
		copy_m3_m3(tmp, I);
		mul_m3_fl(tmp, -drag);
		add_m3_m3m3(dFdV[i].m, dFdV[i].m, tmp);
#else
		float f[3], tmp[3][3], drag_dfdv[3][3], t[3];
		
		mul_v3_v3fl(f, lV[i], -drag);
		force_world_to_root(t, lX[i], lV[i], f, verts[i].mass, &roots[i]);
		add_v3_v3(lF[i], t);
		
		copy_m3_m3(drag_dfdv, I);
		mul_m3_fl(drag_dfdv, -drag);
		dfdv_world_to_root(tmp, drag_dfdv, verts[i].mass, &roots[i]);
		add_m3_m3m3(dFdV[i].m, dFdV[i].m, tmp);
#endif
	}
#endif
	
	/* handle external forces like wind */
	if (effectors) {
		// 0 = force, 1 = normalized force
		winvec = create_lfvector(cloth->numverts);
		
		if (!winvec)
			printf("winvec: out of memory in implicit.c\n");
		
		// precalculate wind forces
		for (i = 0; i < cloth->numverts; i++) {
			pd_point_from_loc(clmd->scene, (float*)lX[i], (float*)lV[i], i, &epoint);
			pdDoEffectors(effectors, NULL, clmd->sim_parms->effector_weights, &epoint, winvec[i], NULL);
		}
		
		for (i = 0; i < cloth->numfaces; i++) {
			float trinormal[3] = {0, 0, 0}; // normalized triangle normal
			float triunnormal[3] = {0, 0, 0}; // not-normalized-triangle normal
			float tmp[3] = {0, 0, 0};
			float factor = (mfaces[i].v4) ? 0.25 : 1.0 / 3.0;
			factor *= 0.02f;
			
			// calculate face normal
			if (mfaces[i].v4)
				CalcFloat4(lX[mfaces[i].v1], lX[mfaces[i].v2], lX[mfaces[i].v3], lX[mfaces[i].v4], triunnormal);
			else
				CalcFloat(lX[mfaces[i].v1], lX[mfaces[i].v2], lX[mfaces[i].v3], triunnormal);

			normalize_v3_v3(trinormal, triunnormal);
			
			// add wind from v1
			copy_v3_v3(tmp, trinormal);
			mul_v3_fl(tmp, calculateVertexWindForce(winvec[mfaces[i].v1], triunnormal));
			VECADDS(lF[mfaces[i].v1], lF[mfaces[i].v1], tmp, factor);
			
			// add wind from v2
			copy_v3_v3(tmp, trinormal);
			mul_v3_fl(tmp, calculateVertexWindForce(winvec[mfaces[i].v2], triunnormal));
			VECADDS(lF[mfaces[i].v2], lF[mfaces[i].v2], tmp, factor);
			
			// add wind from v3
			copy_v3_v3(tmp, trinormal);
			mul_v3_fl(tmp, calculateVertexWindForce(winvec[mfaces[i].v3], triunnormal));
			VECADDS(lF[mfaces[i].v3], lF[mfaces[i].v3], tmp, factor);
			
			// add wind from v4
			if (mfaces[i].v4) {
				copy_v3_v3(tmp, trinormal);
				mul_v3_fl(tmp, calculateVertexWindForce(winvec[mfaces[i].v4], triunnormal));
				VECADDS(lF[mfaces[i].v4], lF[mfaces[i].v4], tmp, factor);
			}
		}

		/* Hair has only edges */
		if (cloth->numfaces == 0) {
			ClothSpring *spring;
			float edgevec[3] = {0, 0, 0}; //edge vector
			float edgeunnormal[3] = {0, 0, 0}; // not-normalized-edge normal
			float tmp[3] = {0, 0, 0};
			float factor = 0.01;

			search = cloth->springs;
			while (search) {
				spring = search->link;
				
				if (spring->type == CLOTH_SPRING_TYPE_STRUCTURAL) {
					sub_v3_v3v3(edgevec, (float*)lX[spring->ij], (float*)lX[spring->kl]);

					project_v3_v3v3(tmp, winvec[spring->ij], edgevec);
					sub_v3_v3v3(edgeunnormal, winvec[spring->ij], tmp);
					/* hair doesn't stretch too much so we can use restlen pretty safely */
					VECADDS(lF[spring->ij], lF[spring->ij], edgeunnormal, spring->restlen * factor);

					project_v3_v3v3(tmp, winvec[spring->kl], edgevec);
					sub_v3_v3v3(edgeunnormal, winvec[spring->kl], tmp);
					VECADDS(lF[spring->kl], lF[spring->kl], edgeunnormal, spring->restlen * factor);
				}

				search = search->next;
			}
		}

		del_lfvector(winvec);
	}
		
	// calculate spring forces
	search = cloth->springs;
	while (search) {
		// only handle active springs
		ClothSpring *spring = search->link;
		if (!(spring->flags & CLOTH_SPRING_FLAG_DEACTIVATE))
			cloth_calc_spring_force(clmd, search->link, lF, lX, lV, dFdV, dFdX, time);

		search = search->next;
	}
	
	// apply spring forces
	search = cloth->springs;
	while (search) {
		// only handle active springs
		ClothSpring *spring = search->link;
		if (!(spring->flags & CLOTH_SPRING_FLAG_DEACTIVATE))
			cloth_apply_spring_force(clmd, search->link, lF, lX, lV, dFdV, dFdX);
		search = search->next;
	}
	// printf("\n");
}

bool BPH_mass_spring_solve(Implicit_Data *data, float dt)
{
	unsigned int numverts = data->dFdV[0].vcount;
	bool ok;

	lfVector *dFdXmV = create_lfvector(numverts);
	zero_lfvector(data->dV, numverts);

	cp_bfmatrix(data->A, data->M);

	subadd_bfmatrixS_bfmatrixS(data->A, data->dFdV, dt, data->dFdX, (dt*dt));

	mul_bfmatrix_lfvector(dFdXmV, data->dFdX, data->V);

	add_lfvectorS_lfvectorS(data->B, data->F, dt, dFdXmV, (dt*dt), numverts);

	// itstart();

	ok = cg_filtered(data->dV, data->A, data->B, data->z, data->S); /* conjugate gradient algorithm to solve Ax=b */
	// cg_filtered_pre(id->dV, id->A, id->B, id->z, id->S, id->P, id->Pinv, id->bigI);

	// itend();
	// printf("cg_filtered calc time: %f\n", (float)itval());

	// advance velocities
	add_lfvector_lfvector(data->Vnew, data->V, data->dV, numverts);
	// advance positions
	add_lfvector_lfvectorS(data->Xnew, data->X, data->Vnew, dt, numverts);

	del_lfvector(dFdXmV);
	
	return ok;
}

void BPH_mass_spring_apply_result(Implicit_Data *data)
{
	int numverts = data->M[0].vcount;
	cp_lfvector(data->X, data->Xnew, numverts);
	cp_lfvector(data->V, data->Vnew, numverts);
}


/* computes where the cloth would be if it were subject to perfectly stiff edges
 * (edge distance constraints) in a lagrangian solver.  then add forces to help
 * guide the implicit solver to that state.  this function is called after
 * collisions*/
static int UNUSED_FUNCTION(cloth_calc_helper_forces)(Object *UNUSED(ob), ClothModifierData *clmd, float (*initial_cos)[3], float UNUSED(step), float dt)
{
	Cloth *cloth= clmd->clothObject;
	float (*cos)[3] = MEM_callocN(sizeof(float)*3*cloth->numverts, "cos cloth_calc_helper_forces");
	float *masses = MEM_callocN(sizeof(float)*cloth->numverts, "cos cloth_calc_helper_forces");
	LinkNode *node;
	ClothSpring *spring;
	ClothVertex *cv;
	int i, steps;
	
	cv = cloth->verts;
	for (i=0; i<cloth->numverts; i++, cv++) {
		copy_v3_v3(cos[i], cv->tx);
		
		if (cv->goal == 1.0f || len_squared_v3v3(initial_cos[i], cv->tx) != 0.0f) {
			masses[i] = 1e+10;
		}
		else {
			masses[i] = cv->mass;
		}
	}
	
	steps = 55;
	for (i=0; i<steps; i++) {
		for (node=cloth->springs; node; node=node->next) {
			/* ClothVertex *cv1, *cv2; */ /* UNUSED */
			int v1, v2;
			float len, c, l, vec[3];
			
			spring = node->link;
			if (spring->type != CLOTH_SPRING_TYPE_STRUCTURAL && spring->type != CLOTH_SPRING_TYPE_SHEAR) 
				continue;
			
			v1 = spring->ij; v2 = spring->kl;
			/* cv1 = cloth->verts + v1; */ /* UNUSED */
			/* cv2 = cloth->verts + v2; */ /* UNUSED */
			len = len_v3v3(cos[v1], cos[v2]);
			
			sub_v3_v3v3(vec, cos[v1], cos[v2]);
			normalize_v3(vec);
			
			c = (len - spring->restlen);
			if (c == 0.0f)
				continue;
			
			l = c / ((1.0f / masses[v1]) + (1.0f / masses[v2]));
			
			mul_v3_fl(vec, -(1.0f / masses[v1]) * l);
			add_v3_v3(cos[v1], vec);
	
			sub_v3_v3v3(vec, cos[v2], cos[v1]);
			normalize_v3(vec);
			
			mul_v3_fl(vec, -(1.0f / masses[v2]) * l);
			add_v3_v3(cos[v2], vec);
		}
	}
	
	cv = cloth->verts;
	for (i=0; i<cloth->numverts; i++, cv++) {
		float vec[3];
		
		/*compute forces*/
		sub_v3_v3v3(vec, cos[i], cv->tx);
		mul_v3_fl(vec, cv->mass*dt*20.0f);
		add_v3_v3(cv->tv, vec);
		//copy_v3_v3(cv->tx, cos[i]);
	}
	
	MEM_freeN(cos);
	MEM_freeN(masses);
	
	return 1;
}

void BPH_mass_spring_set_root_motion(Implicit_Data *data, int index, const float loc[3], const float vel[3], float rot[3][3], const float angvel[3])
{
	RootTransform *root = &data->root[index];
	
#ifdef CLOTH_ROOT_FRAME
	copy_v3_v3(root->loc, loc);
	copy_v3_v3(root->vel, vel);
	copy_m3_m3(root->rot, rot);
	copy_v3_v3(root->omega, angvel);
	/* XXX root frame acceleration ignored for now */
	zero_v3(root->acc);
	zero_v3(root->domega_dt);
#else
	zero_v3(root->loc);
	zero_v3(root->vel);
	unit_m3(root->rot);
	zero_v3(root->omega);
	zero_v3(root->acc);
	zero_v3(root->domega_dt);
	(void)loc;
	(void)vel;
	(void)rot;
	(void)angvel;
#endif
}

void BPH_mass_spring_set_motion_state(Implicit_Data *data, int index, const float x[3], const float v[3])
{
	RootTransform *root = &data->root[index];
	loc_world_to_root(data->X[index], x, root);
	vel_world_to_root(data->V[index], data->X[index], v, root);
}

void BPH_mass_spring_set_position(Implicit_Data *data, int index, const float x[3])
{
	RootTransform *root = &data->root[index];
	loc_world_to_root(data->X[index], x, root);
}

void BPH_mass_spring_set_velocity(Implicit_Data *data, int index, const float v[3])
{
	RootTransform *root = &data->root[index];
	vel_world_to_root(data->V[index], data->X[index], v, root);
}

void BPH_mass_spring_get_motion_state(struct Implicit_Data *data, int index, float x[3], float v[3])
{
	RootTransform *root = &data->root[index];
	if (x) loc_root_to_world(x, data->X[index], root);
	if (v) vel_root_to_world(v, data->X[index], data->V[index], root);
}

void BPH_mass_spring_set_vertex_mass(Implicit_Data *data, int index, float mass)
{
	unit_m3(data->M[index].m);
	mul_m3_fl(data->M[index].m, mass);
}

int BPH_mass_spring_init_spring(Implicit_Data *data, int index, int v1, int v2)
{
	int s = data->M[0].vcount + index; /* index from array start */
	
	init_fmatrix(data->bigI + s, v1, v2);
	init_fmatrix(data->M + s, v1, v2);
	init_fmatrix(data->dFdX + s, v1, v2);
	init_fmatrix(data->dFdV + s, v1, v2);
	init_fmatrix(data->A + s, v1, v2);
//	init_fmatrix(data->S + s, v1, v2); // has no off-diagonal spring entries
	init_fmatrix(data->P + s, v1, v2);
	init_fmatrix(data->Pinv + s, v1, v2);
	
	return s;
}

void BPH_mass_spring_clear_constraints(Implicit_Data *data)
{
	int i, numverts = data->S[0].vcount;
	for (i = 0; i < numverts; ++i) {
		unit_m3(data->S[i].m);
		zero_v3(data->z[i]);
	}
}

void BPH_mass_spring_add_constraint_ndof0(Implicit_Data *data, int index, const float dV[3])
{
	RootTransform *root = &data->root[index];
	
	zero_m3(data->S[index].m);
	
	copy_v3_v3(data->z[index], dV);
	mul_transposed_m3_v3(root->rot, data->z[index]);
}

void BPH_mass_spring_add_constraint_ndof1(Implicit_Data *data, int index, const float c1[3], const float c2[3], const float dV[3])
{
	RootTransform *root = &data->root[index];
	float m[3][3], p[3], q[3], u[3], cmat[3][3];
	
	copy_v3_v3(p, c1);
	mul_transposed_m3_v3(root->rot, p);
	mul_fvectorT_fvector(cmat, p, p);
	sub_m3_m3m3(m, I, cmat);
	
	copy_v3_v3(q, c2);
	mul_transposed_m3_v3(root->rot, q);
	mul_fvectorT_fvector(cmat, q, q);
	sub_m3_m3m3(m, m, cmat);
	
	/* XXX not sure but multiplication should work here */
	copy_m3_m3(data->S[index].m, m);
//	mul_m3_m3m3(data->S[index].m, data->S[index].m, m);
	
	copy_v3_v3(u, dV);
	mul_transposed_m3_v3(root->rot, u);
	add_v3_v3(data->z[index], u);
}

void BPH_mass_spring_add_constraint_ndof2(Implicit_Data *data, int index, const float c1[3], const float dV[3])
{
	RootTransform *root = &data->root[index];
	float m[3][3], p[3], u[3], cmat[3][3];
	
	copy_v3_v3(p, c1);
	mul_transposed_m3_v3(root->rot, p);
	mul_fvectorT_fvector(cmat, p, p);
	sub_m3_m3m3(m, I, cmat);
	
	copy_m3_m3(data->S[index].m, m);
//	mul_m3_m3m3(data->S[index].m, data->S[index].m, m);
	
	copy_v3_v3(u, dV);
	mul_transposed_m3_v3(root->rot, u);
	add_v3_v3(data->z[index], u);
}

#endif /* IMPLICIT_SOLVER_BLENDER */