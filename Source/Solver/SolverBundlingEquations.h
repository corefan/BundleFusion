#pragma once

#ifndef _SOLVER_EQUATIONS_
#define _SOLVER_EQUATIONS_


#include "GlobalDefines.h"
#include <math_constants.h>

#ifndef USE_LIE_SPACE

#define THREADS_PER_BLOCK_JT 128

#include <cutil_inline.h>
#include <cutil_math.h>

#include "../SiftGPU/cuda_SimpleMatrixUtil.h"

#include "SolverBundlingUtil.h"
#include "SolverBundlingState.h"
#include "SolverBundlingParameters.h"

#include "ICPUtil.h"

// residual functions only for sparse!

// not squared!
__inline__ __device__ float evalAbsResidualDeviceFloat3(unsigned int corrIdx, unsigned int componentIdx, SolverInput& input, SolverState& state, SolverParameters& parameters)
{
	float3 r = make_float3(0.0f, 0.0f, 0.0f);

	const EntryJ& corr = input.d_correspondences[corrIdx];
	if (corr.isValid()) {
		float3x3 TI = evalRMat(state.d_xRot[corr.imgIdx_i]);
		float3x3 TJ = evalRMat(state.d_xRot[corr.imgIdx_j]);

		r = parameters.weightSparse * fabs((TI*corr.pos_i + state.d_xTrans[corr.imgIdx_i]) - (TJ*corr.pos_j + state.d_xTrans[corr.imgIdx_j]));
		if (componentIdx == 0) return r.x;
		if (componentIdx == 1) return r.y;
		return r.z; //if (componentIdx == 2) 
	}
	return 0.0f;
}

__inline__ __device__ float evalFDevice(unsigned int corrIdx, SolverInput& input, SolverState& state, SolverParameters& parameters, bool debug)
{
	float3 r = make_float3(0.0f, 0.0f, 0.0f);

	const EntryJ& corr = input.d_correspondences[corrIdx];
	if (corr.isValid()) {
		float3x3 TI = evalRMat(state.d_xRot[corr.imgIdx_i]);
		float3x3 TJ = evalRMat(state.d_xRot[corr.imgIdx_j]);

		r = (TI*corr.pos_i + state.d_xTrans[corr.imgIdx_i]) - (TJ*corr.pos_j + state.d_xTrans[corr.imgIdx_j]);

		float res = parameters.weightSparse * dot(r, r);
		return res;
	}
	return 0.0f;
}


////////////////////////////////////////
// applyJT : this function is called per variable and evaluates each residual influencing that variable (i.e., each energy term per variable)
////////////////////////////////////////

__inline__ __device__ void evalMinusJTFDevice(unsigned int variableIdx, SolverInput& input, SolverState& state, SolverParameters& parameters, float3& resRot, float3& resTrans)
{
	float3 rRot = make_float3(0.0f, 0.0f, 0.0f);
	float3 rTrans = make_float3(0.0f, 0.0f, 0.0f);

	float3 pRot = make_float3(0.0f, 0.0f, 0.0f);
	float3 pTrans = make_float3(0.0f, 0.0f, 0.0f);

	// Reset linearized update vector
	state.d_deltaRot[variableIdx] = make_float3(0.0f, 0.0f, 0.0f);
	state.d_deltaTrans[variableIdx] = make_float3(0.0f, 0.0f, 0.0f);

	// Compute -JTF here
	int N = min(input.d_numEntriesPerRow[variableIdx], input.maxCorrPerImage);

	const float3&  oldAngles0 = state.d_xRot[variableIdx]; // get angles
	const float3x3 R_dAlpha = evalR_dAlpha(oldAngles0);
	const float3x3 R_dBeta = evalR_dBeta(oldAngles0);
	const float3x3 R_dGamma = evalR_dGamma(oldAngles0);

	for (int i = 0; i < N; i++)
	{
		int corrIdx = input.d_variablesToCorrespondences[variableIdx*input.maxCorrPerImage + i];
		const EntryJ& corr = input.d_correspondences[corrIdx];
		if (corr.isValid()) {
			float3 variableP = corr.pos_i;
			float  variableSign = 1;
			if (variableIdx != corr.imgIdx_i)
			{
				variableP = corr.pos_j;
				variableSign = -1;
			}

			const float3x3 TI = evalRMat(state.d_xRot[corr.imgIdx_i]);
			const float3x3 TJ = evalRMat(state.d_xRot[corr.imgIdx_j]);
			const float3 r = (TI*corr.pos_i + state.d_xTrans[corr.imgIdx_i]) - (TJ*corr.pos_j + state.d_xTrans[corr.imgIdx_j]);

			rRot += variableSign*make_float3(dot(R_dAlpha*variableP, r), dot(R_dBeta*variableP, r), dot(R_dGamma*variableP, r));
			rTrans += variableSign*r;

			pRot += make_float3(dot(R_dAlpha*variableP, R_dAlpha*variableP), dot(R_dBeta*variableP, R_dBeta*variableP), dot(R_dGamma*variableP, R_dGamma*variableP));
			pTrans += make_float3(1.0f, 1.0f, 1.0f);
		}
	}
	resRot = -parameters.weightSparse * rRot;
	resTrans = -parameters.weightSparse * rTrans;
	pRot *= parameters.weightSparse;
	pTrans *= parameters.weightSparse;

	// add dense term
	uint3 rotIndices = make_uint3(variableIdx * 6 + 0, variableIdx * 6 + 1, variableIdx * 6 + 2);
	uint3 transIndices = make_uint3(variableIdx * 6 + 3, variableIdx * 6 + 4, variableIdx * 6 + 5);
	resRot -= make_float3(state.d_denseJtr[rotIndices.x], state.d_denseJtr[rotIndices.y], state.d_denseJtr[rotIndices.z]); //minus since -Jtf, weight already built in
	resTrans -= make_float3(state.d_denseJtr[transIndices.x], state.d_denseJtr[transIndices.y], state.d_denseJtr[transIndices.z]); //minus since -Jtf, weight already built in
	// preconditioner
	pRot += make_float3(
		state.d_denseJtJ[rotIndices.x * input.numberOfImages * 6 + rotIndices.x],
		state.d_denseJtJ[rotIndices.y * input.numberOfImages * 6 + rotIndices.y],
		state.d_denseJtJ[rotIndices.z * input.numberOfImages * 6 + rotIndices.z]);
	pTrans += make_float3(
		state.d_denseJtJ[transIndices.x * input.numberOfImages * 6 + transIndices.x],
		state.d_denseJtJ[transIndices.y * input.numberOfImages * 6 + transIndices.y],
		state.d_denseJtJ[transIndices.z * input.numberOfImages * 6 + transIndices.z]);
	// end dense part

	// Preconditioner depends on last solution P(input.d_x)
	if (pRot.x > FLOAT_EPSILON)   state.d_precondionerRot[variableIdx].x = 1.0f / pRot.x;
	else					      state.d_precondionerRot[variableIdx].x = 1.0f;

	if (pRot.y > FLOAT_EPSILON)   state.d_precondionerRot[variableIdx].y = 1.0f / pRot.y;
	else					      state.d_precondionerRot[variableIdx].y = 1.0f;

	if (pRot.z > FLOAT_EPSILON)   state.d_precondionerRot[variableIdx].z = 1.0f / pRot.z;
	else						  state.d_precondionerRot[variableIdx].z = 1.0f;

	if (pTrans.x > FLOAT_EPSILON) state.d_precondionerTrans[variableIdx].x = 1.0f / pTrans.x;
	else					      state.d_precondionerTrans[variableIdx].x = 1.0f;

	if (pTrans.y > FLOAT_EPSILON) state.d_precondionerTrans[variableIdx].y = 1.0f / pTrans.y;
	else					      state.d_precondionerTrans[variableIdx].y = 1.0f;

	if (pTrans.z > FLOAT_EPSILON) state.d_precondionerTrans[variableIdx].z = 1.0f / pTrans.z;
	else					      state.d_precondionerTrans[variableIdx].z = 1.0f;
}

////////////////////////////////////////
// applyJT : this function is called per variable and evaluates each residual influencing that variable (i.e., each energy term per variable)
////////////////////////////////////////

__inline__ __device__ void applyJTDevice(unsigned int variableIdx, SolverInput& input, SolverState& state, const SolverParameters& parameters,
	float3& outRot, float3& outTrans, unsigned int threadIdx, unsigned int lane)
{
	// Compute J^T*d_Jp here
	outRot = make_float3(0.0f, 0.0f, 0.0f);
	outTrans = make_float3(0.0f, 0.0f, 0.0f);

	const float3&  oldAngles0 = state.d_xRot[variableIdx]; // get angles
	const float3x3 R_dAlpha = evalR_dAlpha(oldAngles0);
	const float3x3 R_dBeta = evalR_dBeta(oldAngles0);
	const float3x3 R_dGamma = evalR_dGamma(oldAngles0);

	int N = min(input.d_numEntriesPerRow[variableIdx], input.maxCorrPerImage);

	for (int i = threadIdx; i < N; i += THREADS_PER_BLOCK_JT)
	{
		int corrIdx = input.d_variablesToCorrespondences[variableIdx*input.maxCorrPerImage + i];
		const EntryJ& corr = input.d_correspondences[corrIdx];
		if (corr.isValid()) {
			float3 variableP = corr.pos_i;
			float  variableSign = 1;
			if (variableIdx != corr.imgIdx_i)
			{
				variableP = corr.pos_j;
				variableSign = -1;
			}

			outRot += variableSign * make_float3(dot(R_dAlpha*variableP, state.d_Jp[corrIdx]), dot(R_dBeta*variableP, state.d_Jp[corrIdx]), dot(R_dGamma*variableP, state.d_Jp[corrIdx]));
			outTrans += variableSign * state.d_Jp[corrIdx];
		}
	}
	//apply j already applied the weight

	outRot.x = warpReduce(outRot.x);	 outRot.y = warpReduce(outRot.y);	  outRot.z = warpReduce(outRot.z);
	outTrans.x = warpReduce(outTrans.x); outTrans.y = warpReduce(outTrans.y); outTrans.z = warpReduce(outTrans.z);
}

__inline__ __device__ float3 applyJDevice(unsigned int corrIdx, SolverInput& input, SolverState& state, const SolverParameters& parameters)
{
	// Compute Jp here
	float3 b = make_float3(0.0f, 0.0f, 0.0f);
	const EntryJ& corr = input.d_correspondences[corrIdx];

	if (corr.isValid()) {
		if (corr.imgIdx_i > 0)	// get transform 0
		{
			const float3& oldAngles0 = state.d_xRot[corr.imgIdx_i]; // get angles
			const float3  dAlpha0 = evalR_dAlpha(oldAngles0)*corr.pos_i;
			const float3  dBeta0 = evalR_dBeta(oldAngles0)*corr.pos_i;
			const float3  dGamma0 = evalR_dGamma(oldAngles0)*corr.pos_i;
			const float3  pp0 = state.d_pRot[corr.imgIdx_i];
			b += dAlpha0*pp0.x + dBeta0*pp0.y + dGamma0*pp0.z + state.d_pTrans[corr.imgIdx_i];
		}

		if (corr.imgIdx_j > 0)	// get transform 1
		{
			const float3& oldAngles1 = state.d_xRot[corr.imgIdx_j]; // get angles
			const float3  dAlpha1 = evalR_dAlpha(oldAngles1)*corr.pos_j;
			const float3  dBeta1 = evalR_dBeta(oldAngles1)*corr.pos_j;
			const float3  dGamma1 = evalR_dGamma(oldAngles1)*corr.pos_j;
			const float3  pp1 = state.d_pRot[corr.imgIdx_j];
			b -= dAlpha1*pp1.x + dBeta1*pp1.y + dGamma1*pp1.z + state.d_pTrans[corr.imgIdx_j];
		}
		b *= parameters.weightSparse;
	}
	return b;
}

////////////////////////////////////////
// dense depth term
////////////////////////////////////////

__inline__ __device__ void computeJacobianBlockRow_i(matNxM<1, 6>& jacBlockRow, const float3& angles, const float3& translation,
	const float4x4& transform_j, const float4& camPosSrc, const float4& normalTgt)
{
	//!!!DEBUGGING
	if (isnan(camPosSrc.x) || isnan(camPosSrc.y) || isnan(camPosSrc.z) || isnan(camPosSrc.w)) {
		printf("ERROR jac i: camPosSrc = %f %f %f %f\n", camPosSrc.x, camPosSrc.y, camPosSrc.z, camPosSrc.w);
	}
	if (isnan(normalTgt.x) || isnan(normalTgt.y) || isnan(normalTgt.z) || isnan(normalTgt.w)) {
		printf("ERROR jac i: camPosSrc = %f %f %f %f\n", normalTgt.x, normalTgt.y, normalTgt.z, normalTgt.w);
	}
	//!!!DEBUGGING

	float4 world = transform_j * camPosSrc;
	//alpha
	float4x4 dx = evalRtInverse_dAlpha(angles, translation);
	jacBlockRow(0) = -dot(dx * world, normalTgt);
	//beta
	dx = evalRtInverse_dBeta(angles, translation);
	jacBlockRow(1) = -dot(dx * world, normalTgt);
	//gamma
	dx = evalRtInverse_dGamma(angles, translation);
	jacBlockRow(2) = -dot(dx * world, normalTgt);
	//x
	dx = evalRtInverse_dX(angles, translation);
	jacBlockRow(3) = -dot(dx * world, normalTgt);
	//y
	dx = evalRtInverse_dY(angles, translation);
	jacBlockRow(4) = -dot(dx * world, normalTgt);
	//z
	dx = evalRtInverse_dZ(angles, translation);
	jacBlockRow(5) = -dot(dx * world, normalTgt);
}
__inline__ __device__ void computeJacobianBlockRow_j(matNxM<1, 6>& jacBlockRow, const float3& angles, const float3& translation,
	const float4x4& invTransform_i, const float4& camPosSrc, const float4& normalTgt)
{
	//!!!DEBUGGING
	if (isnan(camPosSrc.x) || isnan(camPosSrc.y) || isnan(camPosSrc.z) || isnan(camPosSrc.w)) {
		printf("ERROR jac j: camPosSrc = %f %f %f %f\n", camPosSrc.x, camPosSrc.y, camPosSrc.z, camPosSrc.w);
	}
	if (isnan(normalTgt.x) || isnan(normalTgt.y) || isnan(normalTgt.z) || isnan(normalTgt.w)) {
		printf("ERROR jac j: camPosSrc = %f %f %f %f\n", normalTgt.x, normalTgt.y, normalTgt.z, normalTgt.w);
	}
	//!!!DEBUGGING

	float4x4 dx; dx.setIdentity();
	//alpha
	dx.setFloat3x3(evalR_dAlpha(angles));
	jacBlockRow(0) = -dot(invTransform_i * dx * camPosSrc, normalTgt);
	//beta
	dx.setFloat3x3(evalR_dBeta(angles));
	jacBlockRow(1) = -dot(invTransform_i * dx * camPosSrc, normalTgt);
	//gamma
	dx.setFloat3x3(evalR_dGamma(angles));
	jacBlockRow(2) = -dot(invTransform_i * dx * camPosSrc, normalTgt);
	//x
	float4 dt = make_float4(1.0f, 0.0f, 0.0f, 1.0f);
	jacBlockRow(3) = -dot(invTransform_i * dt, normalTgt);
	//y
	dt = make_float4(0.0f, 1.0f, 0.0f, 1.0f);
	jacBlockRow(4) = -dot(invTransform_i * dt, normalTgt);
	//z
	dt = make_float4(0.0f, 0.0f, 1.0f, 1.0f);
	jacBlockRow(5) = -dot(invTransform_i * dt, normalTgt);
}
////////////////////////////////////////
// dense depth term
////////////////////////////////////////
__inline__ __device__ float computeColorDProjLookup(const float4& dx, const float4& camPosSrcToTgt, const float2& intensityDerivTgt, const float2& colorFocalLength)
{
	mat3x1 dcdx; dcdx(0) = dx.x; dcdx(1) = dx.y; dcdx(2) = dx.z;
	mat2x3 dProjectionC = dCameraToScreen(camPosSrcToTgt, colorFocalLength.x, colorFocalLength.y);
	mat1x2 dColorB(intensityDerivTgt);
	mat1x1 dadx = dColorB * dProjectionC * dcdx;

	//if (debug) {
	//	mat1x3 dd = dColorB * dProjectionC;
	//	printf("dcdx = %f %f %f\n", dcdx(0), dcdx(1), dcdx(2));
	//	printf("dProjectionC = %f %f %f, %f %f %f\n", dProjectionC(0, 0), dProjectionC(0, 1), dProjectionC(0, 2),
	//		dProjectionC(1, 0), dProjectionC(1, 1), dProjectionC(1, 2));
	//	printf("dintensity*dProj = %f %f %f\n", dd(0), dd(1), dd(2));
	//	printf("dbdx = %f %f\n", dbdx(0), dbdx(1));
	//	printf("dintensity = %f %f\n", dColorB(0), dColorB(1));
	//	printf("res = %f\n", dadx(0));
	//	printf("\n");
	//}

	return dadx(0);
}
__inline__ __device__ void computeJacobianBlockIntensityRow_i(matNxM<1, 6>& jacBlockRow, const float2& colorFocal, const float3& angles, const float3& translation,
	const float4x4& transform_j, const float4& camPosSrc, const float4& camPosSrcToTgt, const float2& intensityDerivTgt)
{
	float4 world = transform_j * camPosSrc;
	//alpha
	float4 dx = evalRtInverse_dAlpha(angles, translation) * world;
	jacBlockRow(0) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	//beta
	dx = evalRtInverse_dBeta(angles, translation) * world;
	jacBlockRow(1) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	//gamma
	dx = evalRtInverse_dGamma(angles, translation) * world;
	jacBlockRow(2) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	//x
	dx = evalRtInverse_dX(angles, translation) * world;
	jacBlockRow(3) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	//y
	dx = evalRtInverse_dY(angles, translation) * world;
	jacBlockRow(4) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	//z
	dx = evalRtInverse_dZ(angles, translation) * world;
	jacBlockRow(5) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
}
__inline__ __device__ void computeJacobianBlockIntensityRow_j(matNxM<1, 6>& jacBlockRow, const float2& colorFocal, const float3& angles, const float3& translation,
	const float4x4& invTransform_i, const float4& camPosSrc, const float4& camPosSrcToTgt, const float2& intensityDerivTgt)
{
	//alpha
	float4 dx = invTransform_i * evalR_dAlpha(angles) * camPosSrc;
	jacBlockRow(0) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	//beta
	dx = invTransform_i * evalR_dBeta(angles) * camPosSrc;
	jacBlockRow(1) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	//gamma
	dx = invTransform_i * evalR_dGamma(angles) * camPosSrc;
	jacBlockRow(2) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	//x
	dx = invTransform_i * make_float4(1.0f, 0.0f, 0.0f, 1.0f);
	jacBlockRow(3) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	//y
	dx = invTransform_i * make_float4(0.0f, 1.0f, 0.0f, 1.0f);
	jacBlockRow(4) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	//z
	dx = invTransform_i * make_float4(0.0f, 0.0f, 1.0f, 1.0f);
	jacBlockRow(5) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
}
#endif
////////////////////////////////////////
// dense term
////////////////////////////////////////

__inline__ __device__ void addToLocalSystem(float* d_JtJ, float* d_Jtr, unsigned int dim, const matNxM<1, 6>& jacobianBlockRow_i, const matNxM<1, 6>& jacobianBlockRow_j,
	unsigned int vi, unsigned int vj, float residual, float weight)
{
	//fill in bottom half (vi < vj) -> x < y
	for (unsigned int i = 0; i < 6; i++) {
		for (unsigned int j = i; j < 6; j++) {
			if (vi > 0) {
				float dii = jacobianBlockRow_i(i) * jacobianBlockRow_i(j) * weight;
				//!!!DEBUGGING
				if (isnan(dii)) printf("ERROR addtolocalsystem (%d,%d)(%d,%d) %f %f %f\n", vi, vj, i, j, jacobianBlockRow_i(i), jacobianBlockRow_i(j), weight);
				//!!!DEBUGGING
				atomicAdd(&d_JtJ[(vi * 6 + j)*dim + (vi * 6 + i)], dii);
			}
			if (vj > 0) {
				float djj = jacobianBlockRow_j(i) * jacobianBlockRow_j(j) * weight;
				//!!!DEBUGGING
				if (isnan(djj)) printf("ERROR addtolocalsystem (%d,%d)(%d,%d) %f %f %f\n", vi, vj, i, j, jacobianBlockRow_j(i), jacobianBlockRow_j(j), weight);
				//!!!DEBUGGING
				atomicAdd(&d_JtJ[(vj * 6 + j)*dim + (vj * 6 + i)], djj);
			}
			if (vi > 0 && vj > 0) {
				float dij = jacobianBlockRow_i(i) * jacobianBlockRow_j(j) * weight;
				//!!!DEBUGGING
				if (isnan(dij)) printf("ERROR addtolocalsystem (%d,%d)(%d,%d) %f %f %f\n", vi, vj, i, j, jacobianBlockRow_i(i), jacobianBlockRow_j(j), weight);
				//!!!DEBUGGING
				atomicAdd(&d_JtJ[(vj * 6 + j)*dim + (vi * 6 + i)], dij);
				if (i != j)	{
					float dji = jacobianBlockRow_i(j) * jacobianBlockRow_j(i) * weight;
					//!!!DEBUGGING
					if (isnan(dji)) printf("ERROR addtolocalsystem (%d,%d)(%d,%d) %f %f %f\n", vi, vj, i, j, jacobianBlockRow_i(j), jacobianBlockRow_j(i), weight);
					//!!!DEBUGGING
					atomicAdd(&d_JtJ[(vj * 6 + i)*dim + (vi * 6 + j)], dji);
				}
			}
		}
		if (vi > 0) atomicAdd(&d_Jtr[vi * 6 + i], jacobianBlockRow_i(i) * residual * weight);
		if (vj > 0) atomicAdd(&d_Jtr[vj * 6 + i], jacobianBlockRow_j(i) * residual * weight);
	}
}

//TODO MAKE EFFICIENT
__inline__ __device__ void applyJTJDevice(unsigned int variableIdx, SolverState& state, float* d_JtJ, unsigned int N, float3& outRot, float3& outTrans)
{
	// Compute J^T*d_Jp here
	outRot = make_float3(0.0f, 0.0f, 0.0f);
	outTrans = make_float3(0.0f, 0.0f, 0.0f);

	const unsigned int dim = 6 * N;

	unsigned int baseVarIdx = variableIdx * 6;
	for (unsigned int i = 1; i < N; i++) // iterate through (6) row(s) of JtJ and all of p (i = 0 not a variable)
	{
		// (row, col) = vars, i
		unsigned int baseIdx = 6 * i;

		float3x3 block00(
			d_JtJ[(baseVarIdx + 0)* dim + baseIdx + 0], d_JtJ[(baseVarIdx + 0)* dim + baseIdx + 1], d_JtJ[(baseVarIdx + 0)* dim + baseIdx + 2],
			d_JtJ[(baseVarIdx + 1)* dim + baseIdx + 0], d_JtJ[(baseVarIdx + 1)* dim + baseIdx + 1], d_JtJ[(baseVarIdx + 1)* dim + baseIdx + 2],
			d_JtJ[(baseVarIdx + 2)* dim + baseIdx + 0], d_JtJ[(baseVarIdx + 2)* dim + baseIdx + 1], d_JtJ[(baseVarIdx + 2)* dim + baseIdx + 2]);
		float3x3 block01(
			d_JtJ[(baseVarIdx + 0)* dim + baseIdx + 3], d_JtJ[(baseVarIdx + 0)* dim + baseIdx + 4], d_JtJ[(baseVarIdx + 0)* dim + baseIdx + 5],
			d_JtJ[(baseVarIdx + 1)* dim + baseIdx + 3], d_JtJ[(baseVarIdx + 1)* dim + baseIdx + 4], d_JtJ[(baseVarIdx + 1)* dim + baseIdx + 5],
			d_JtJ[(baseVarIdx + 2)* dim + baseIdx + 3], d_JtJ[(baseVarIdx + 2)* dim + baseIdx + 4], d_JtJ[(baseVarIdx + 2)* dim + baseIdx + 5]);
		float3x3 block10(
			d_JtJ[(baseVarIdx + 3)* dim + baseIdx + 0], d_JtJ[(baseVarIdx + 3)* dim + baseIdx + 1], d_JtJ[(baseVarIdx + 3)* dim + baseIdx + 2],
			d_JtJ[(baseVarIdx + 4)* dim + baseIdx + 0], d_JtJ[(baseVarIdx + 4)* dim + baseIdx + 1], d_JtJ[(baseVarIdx + 4)* dim + baseIdx + 2],
			d_JtJ[(baseVarIdx + 5)* dim + baseIdx + 0], d_JtJ[(baseVarIdx + 5)* dim + baseIdx + 1], d_JtJ[(baseVarIdx + 5)* dim + baseIdx + 2]);
		float3x3 block11(
			d_JtJ[(baseVarIdx + 3)* dim + baseIdx + 3], d_JtJ[(baseVarIdx + 3)* dim + baseIdx + 4], d_JtJ[(baseVarIdx + 3)* dim + baseIdx + 5],
			d_JtJ[(baseVarIdx + 4)* dim + baseIdx + 3], d_JtJ[(baseVarIdx + 4)* dim + baseIdx + 4], d_JtJ[(baseVarIdx + 4)* dim + baseIdx + 5],
			d_JtJ[(baseVarIdx + 5)* dim + baseIdx + 3], d_JtJ[(baseVarIdx + 5)* dim + baseIdx + 4], d_JtJ[(baseVarIdx + 5)* dim + baseIdx + 5]);

		//outRot += block00 * state.d_pRot[i] + block01 * state.d_pTrans[i];
		//outTrans += block10 * state.d_pRot[i] + block11 * state.d_pTrans[i];
		outTrans += block00 * state.d_pTrans[i] + block01 * state.d_pRot[i];
		outRot += block10 * state.d_pTrans[i] + block11 * state.d_pRot[i];
	}
}
//__inline__ __device__ void applyJTJDenseDevice(unsigned int variableIdx, SolverInput& input, SolverState& state, const SolverParameters& parameters,
//	float3& outRot, float3& outTrans, unsigned int threadIdx)
//{
//	// Compute J^T*d_Jp here
//	outRot = make_float3(0.0f, 0.0f, 0.0f);
//	outTrans = make_float3(0.0f, 0.0f, 0.0f);
//
//	int N = input.numberOfImages;
//
//	for (int i = threadIdx; i < N; i += THREADS_PER_BLOCK_JT)
//	{
//		//(row,col) = (variableIdx + (3/3), i + (3/3))
//		float3 rotPart = make_float3(
//			state.d_depthJtJ[variableIdx * 6 * N + i * 6 + 0],
//			state.d_depthJtJ[variableIdx * 6 * N + i * 6 + 1],
//			state.d_depthJtJ[variableIdx * 6 * N + i * 6 + 2]);
//		float3 transPart = make_float3(
//			state.d_depthJtJ[variableIdx * 6 * N + i * 6 + 3],
//			state.d_depthJtJ[variableIdx * 6 * N + i * 6 + 4],
//			state.d_depthJtJ[variableIdx * 6 * N + i * 6 + 5]);
//		outRot += dot(rotPart, state.d_pRot[i]);
//		outTrans += dot(transPart, state.d_pTrans[i]);
//	}
//
//	outRot.x = warpReduce(outRot.x);	 outRot.y = warpReduce(outRot.y);	  outRot.z = warpReduce(outRot.z);
//	outTrans.x = warpReduce(outTrans.x); outTrans.y = warpReduce(outTrans.y); outTrans.z = warpReduce(outTrans.z);
//}

///////////////////////////////////////////////////////////////////
// camera functions
///////////////////////////////////////////////////////////////////
__inline__ __device__ bool computeAngleDiff(const float4x4& transform, float angleThresh)
{
	float3 x = normalize(make_float3(1.0f, 1.0f, 1.0f)); 
	float3 v = transform.getFloat3x3() * x;
	float angle = acos(clamp(dot(x, v), -1.0f, 1.0f));
	if (fabs(angle) < angleThresh) return true;
	return false;

	// decompose to z-y-x euler angles
	//const float eps = 0.0001f;

	//float psi, theta, phi; // x,y,z axis angles
	//if (transform(2, 0) > -1 + eps && transform(2, 0) < 1 - eps) { // R(2, 0) != +/- 1
	//	theta = -asin(transform(2, 0)); // \pi - theta
	//	float costheta = cos(theta);
	//	psi = atan2(transform(2, 1) / costheta, transform(2, 2) / costheta);
	//	phi = atan2(transform(1, 0) / costheta, transform(0, 0) / costheta);
	//}
	//else {
	//	phi = 0;
	//	if (transform(2, 0) <= -1 + eps) {
	//		theta = CUDART_PI_F / 2.0f;
	//		psi = phi + atan2(transform(0, 1), transform(0, 2));
	//	}
	//	else {	// R(2, 0) == 1
	//		theta = -CUDART_PI_F / 2.0f;
	//		psi = -phi + atan2(-transform(0, 1), -transform(0, 2));
	//	}
	//}

	//float3 rot = make_float3(psi, theta, phi);
	//if (fabs(rot.x) < angleThresh && fabs(rot.y) < angleThresh && fabs(rot.z) < angleThresh)
	//	return true;
	//return false;
}

#endif
