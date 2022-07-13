#include "precomp.h"
#include "bvh.h"

/*
Performance: 1858ms without kD-tree
with kDtree:
- 15s without culling
- 8.1s with culling
- 3.4s without removeLeaf refitting
- 858ms with recursive refitting
- 836ms with cache alignment
*/

// functions

void IntersectTri( Ray& ray, const Tri& tri, const uint instPrim )
{
	// Moeller-Trumbore ray/triangle intersection algorithm, see:
	// en.wikipedia.org/wiki/M%C3%B6ller%E2%80%93Trumbore_intersection_algorithm
	const float3 edge1 = tri.vertex1 - tri.vertex0;
	const float3 edge2 = tri.vertex2 - tri.vertex0;
	const float3 h = cross( ray.D, edge2 );
	const float a = dot( edge1, h );
	if (fabs( a ) < 0.00001f) return; // ray parallel to triangle
	const float f = 1 / a;
	const float3 s = ray.O - tri.vertex0;
	const float u = f * dot( s, h );
	if (u < 0 || u > 1) return;
	const float3 q = cross( s, edge1 );
	const float v = f * dot( ray.D, q );
	if (v < 0 || u + v > 1) return;
	const float t = f * dot( edge2, q );
	if (t > 0.0001f && t < ray.hit.t)
		ray.hit.t = t, ray.hit.u = u,
		ray.hit.v = v, ray.hit.instPrim = instPrim;
}

inline float IntersectAABB( const Ray& ray, const float3 bmin, const float3 bmax )
{
	// "slab test" ray/AABB intersection
	float tx1 = (bmin.x - ray.O.x) * ray.rD.x, tx2 = (bmax.x - ray.O.x) * ray.rD.x;
	float tmin = min( tx1, tx2 ), tmax = max( tx1, tx2 );
	float ty1 = (bmin.y - ray.O.y) * ray.rD.y, ty2 = (bmax.y - ray.O.y) * ray.rD.y;
	tmin = max( tmin, min( ty1, ty2 ) ), tmax = min( tmax, max( ty1, ty2 ) );
	float tz1 = (bmin.z - ray.O.z) * ray.rD.z, tz2 = (bmax.z - ray.O.z) * ray.rD.z;
	tmin = max( tmin, min( tz1, tz2 ) ), tmax = min( tmax, max( tz1, tz2 ) );
	if (tmax >= tmin && tmin < ray.hit.t && tmax > 0) return tmin; else return 1e30f;
}

float IntersectAABB_SSE( const Ray& ray, const __m128& bmin4, const __m128& bmax4 )
{
	// "slab test" ray/AABB intersection, using SIMD instructions
	static __m128 mask4 = _mm_cmpeq_ps( _mm_setzero_ps(), _mm_set_ps( 1, 0, 0, 0 ) );
	__m128 t1 = _mm_mul_ps( _mm_sub_ps( _mm_and_ps( bmin4, mask4 ), ray.O4 ), ray.rD4 );
	__m128 t2 = _mm_mul_ps( _mm_sub_ps( _mm_and_ps( bmax4, mask4 ), ray.O4 ), ray.rD4 );
	__m128 vmax4 = _mm_max_ps( t1, t2 ), vmin4 = _mm_min_ps( t1, t2 );
	float tmax = min( vmax4.m128_f32[0], min( vmax4.m128_f32[1], vmax4.m128_f32[2] ) );
	float tmin = max( vmin4.m128_f32[0], max( vmin4.m128_f32[1], vmin4.m128_f32[2] ) );
	if (tmax >= tmin && tmin < ray.hit.t && tmax > 0) return tmin; else return 1e30f;
}

// Mesh class implementation

Mesh::Mesh( const char* objFile, const char* texFile )
{
	// bare-bones obj file loader; only supports very basic meshes
	texture = new Surface( texFile );
	tri = new Tri[19500];
	triEx = new TriEx[19500];
	float2* UV = new float2[11042]; // enough for dragon.obj
	N = new float3[11042], P = new float3[11042];
	int UVs = 0, Ns = 0, Ps = 0, a, b, c, d, e, f, g, h, i;
	FILE* file = fopen( objFile, "r" );
	while (!feof( file ))
	{
		char line[512] = { 0 };
		fgets( line, 511, file );
		if (line == strstr( line, "vt " ))
			sscanf( line + 3, "%f %f", &UV[UVs].x, &UV[UVs].y ), UVs++;
		else if (line == strstr( line, "vn " ))
			sscanf( line + 3, "%f %f %f", &N[Ns].x, &N[Ns].y, &N[Ns].z ), Ns++;
		else if (line[0] == 'v')
			sscanf( line + 2, "%f %f %f", &P[Ps].x, &P[Ps].y, &P[Ps].z ), Ps++;
		if (line[0] != 'f') continue; else
			sscanf( line + 2, "%i/%i/%i %i/%i/%i %i/%i/%i",
				&a, &b, &c, &d, &e, &f, &g, &h, &i );
		tri[triCount].vertex0 = P[a - 1], triEx[triCount].N0 = N[c - 1];
		tri[triCount].vertex1 = P[d - 1], triEx[triCount].N1 = N[f - 1];
		tri[triCount].vertex2 = P[g - 1], triEx[triCount].N2 = N[i - 1];
		triEx[triCount].uv0 = UV[b - 1], triEx[triCount].uv1 = UV[e - 1];
		triEx[triCount++].uv2 = UV[h - 1];
	}
	fclose( file );
	bvh = new BVH( this );
}

// BVH class implementation

BVH::BVH( Mesh* triMesh )
{
	mesh = triMesh;
	bvhNode = (BVHNode*)_aligned_malloc( sizeof( BVHNode ) * mesh->triCount * 2, 64 );
	triIdx = new uint[mesh->triCount];
	Build();
}

void BVH::Intersect( Ray& ray, uint instanceIdx )
{
	BVHNode* node = &bvhNode[0], * stack[64];
	uint stackPtr = 0;
	while (1)
	{
		if (node->isLeaf())
		{
			for (uint i = 0; i < node->triCount; i++)
			{
				uint instPrim = (instanceIdx << 20) + triIdx[node->leftFirst + i];
				IntersectTri( ray, mesh->tri[instPrim & 0xfffff /* 20 bits */], instPrim );
			}
			if (stackPtr == 0) break; else node = stack[--stackPtr];
			continue;
		}
		BVHNode* child1 = &bvhNode[node->leftFirst];
		BVHNode* child2 = &bvhNode[node->leftFirst + 1];
	#ifdef USE_SSE
		float dist1 = IntersectAABB_SSE( ray, child1->aabbMin4, child1->aabbMax4 );
		float dist2 = IntersectAABB_SSE( ray, child2->aabbMin4, child2->aabbMax4 );
	#else
		float dist1 = IntersectAABB( ray, child1->aabbMin, child1->aabbMax );
		float dist2 = IntersectAABB( ray, child2->aabbMin, child2->aabbMax );
	#endif
		if (dist1 > dist2) { swap( dist1, dist2 ); swap( child1, child2 ); }
		if (dist1 == 1e30f)
		{
			if (stackPtr == 0) break; else node = stack[--stackPtr];
		}
		else
		{
			node = child1;
			if (dist2 != 1e30f) stack[stackPtr++] = child2;
		}
	}
}

void BVH::Refit()
{
	Timer t;
	for (int i = nodesUsed - 1; i >= 0; i--) if (i != 1)
	{
		BVHNode& node = bvhNode[i];
		if (node.isLeaf())
		{
			// leaf node: adjust bounds to contained triangles
			UpdateNodeBounds( i );
			continue;
		}
		// interior node: adjust bounds to child node bounds
		BVHNode& leftChild = bvhNode[node.leftFirst];
		BVHNode& rightChild = bvhNode[node.leftFirst + 1];
		node.aabbMin = fminf( leftChild.aabbMin, rightChild.aabbMin );
		node.aabbMax = fmaxf( leftChild.aabbMax, rightChild.aabbMax );
	}
	printf( "BVH refitted in %.2fms\n", t.elapsed() * 1000 );
}

void BVH::Build()
{
	// reset node pool
	nodesUsed = 2;
	// populate triangle index array
	for (int i = 0; i < mesh->triCount; i++) triIdx[i] = i;
	// calculate triangle centroids for partitioning
	Tri* tri = mesh->tri;
	for (int i = 0; i < mesh->triCount; i++)
		mesh->tri[i].centroid = (tri[i].vertex0 + tri[i].vertex1 + tri[i].vertex2) * 0.3333f;
	// assign all triangles to root node
	BVHNode& root = bvhNode[0];
	root.leftFirst = 0, root.triCount = mesh->triCount;
	UpdateNodeBounds( 0 );
	// subdivide recursively
	Timer t;
	Subdivide( 0 );
	printf( "BVH constructed in %.2fms\n", t.elapsed() * 1000 );
}

void BVH::UpdateNodeBounds( uint nodeIdx )
{
	BVHNode& node = bvhNode[nodeIdx];
	node.aabbMin = float3( 1e30f );
	node.aabbMax = float3( -1e30f );
	for (uint first = node.leftFirst, i = 0; i < node.triCount; i++)
	{
		uint leafTriIdx = triIdx[first + i];
		Tri& leafTri = mesh->tri[leafTriIdx];
		node.aabbMin = fminf( node.aabbMin, leafTri.vertex0 );
		node.aabbMin = fminf( node.aabbMin, leafTri.vertex1 );
		node.aabbMin = fminf( node.aabbMin, leafTri.vertex2 );
		node.aabbMax = fmaxf( node.aabbMax, leafTri.vertex0 );
		node.aabbMax = fmaxf( node.aabbMax, leafTri.vertex1 );
		node.aabbMax = fmaxf( node.aabbMax, leafTri.vertex2 );
	}
}

float BVH::FindBestSplitPlane( BVHNode& node, int& axis, float& splitPos )
{
	float bestCost = 1e30f;
	for (int a = 0; a < 3; a++)
	{
		float boundsMin = 1e30f, boundsMax = -1e30f;
		for (uint i = 0; i < node.triCount; i++)
		{
			Tri& triangle = mesh->tri[triIdx[node.leftFirst + i]];
			boundsMin = min( boundsMin, triangle.centroid[a] );
			boundsMax = max( boundsMax, triangle.centroid[a] );
		}
		if (boundsMin == boundsMax) continue;
		// populate the bins
		struct Bin { aabb bounds; int triCount = 0; } bin[BINS];
		float scale = BINS / (boundsMax - boundsMin);
		for (uint i = 0; i < node.triCount; i++)
		{
			Tri& triangle = mesh->tri[triIdx[node.leftFirst + i]];
			int binIdx = min( BINS - 1, (int)((triangle.centroid[a] - boundsMin) * scale) );
			bin[binIdx].triCount++;
			bin[binIdx].bounds.grow( triangle.vertex0 );
			bin[binIdx].bounds.grow( triangle.vertex1 );
			bin[binIdx].bounds.grow( triangle.vertex2 );
		}
		// gather data for the 7 planes between the 8 bins
		float leftArea[BINS - 1], rightArea[BINS - 1];
		int leftCount[BINS - 1], rightCount[BINS - 1];
		aabb leftBox, rightBox;
		int leftSum = 0, rightSum = 0;
		for (int i = 0; i < BINS - 1; i++)
		{
			leftSum += bin[i].triCount;
			leftCount[i] = leftSum;
			leftBox.grow( bin[i].bounds );
			leftArea[i] = leftBox.area();
			rightSum += bin[BINS - 1 - i].triCount;
			rightCount[BINS - 2 - i] = rightSum;
			rightBox.grow( bin[BINS - 1 - i].bounds );
			rightArea[BINS - 2 - i] = rightBox.area();
		}
		// calculate SAH cost for the 7 planes
		scale = (boundsMax - boundsMin) / BINS;
		for (int i = 0; i < BINS - 1; i++)
		{
			float planeCost = leftCount[i] * leftArea[i] + rightCount[i] * rightArea[i];
			if (planeCost < bestCost)
				axis = a, splitPos = boundsMin + scale * (i + 1), bestCost = planeCost;
		}
	}
	return bestCost;
}

void BVH::Subdivide( uint nodeIdx )
{
	// terminate recursion
	BVHNode& node = bvhNode[nodeIdx];
	// determine split axis using SAH
	int axis;
	float splitPos;
	float splitCost = FindBestSplitPlane( node, axis, splitPos );
	float nosplitCost = node.CalculateNodeCost();
	if (splitCost >= nosplitCost) return;
	// in-place partition
	int i = node.leftFirst;
	int j = i + node.triCount - 1;
	while (i <= j)
	{
		if (mesh->tri[triIdx[i]].centroid[axis] < splitPos)
			i++;
		else
			swap( triIdx[i], triIdx[j--] );
	}
	// abort split if one of the sides is empty
	int leftCount = i - node.leftFirst;
	if (leftCount == 0 || leftCount == node.triCount) return;
	// create child nodes
	int leftChildIdx = nodesUsed++;
	int rightChildIdx = nodesUsed++;
	bvhNode[leftChildIdx].leftFirst = node.leftFirst;
	bvhNode[leftChildIdx].triCount = leftCount;
	bvhNode[rightChildIdx].leftFirst = i;
	bvhNode[rightChildIdx].triCount = node.triCount - leftCount;
	node.leftFirst = leftChildIdx;
	node.triCount = 0;
	UpdateNodeBounds( leftChildIdx );
	UpdateNodeBounds( rightChildIdx );
	// recurse
	Subdivide( leftChildIdx );
	Subdivide( rightChildIdx );
}

// BVHInstance implementation

void BVHInstance::SetTransform( mat4& T )
{
	transform = T;
	transform = T;
	invTransform = transform.Inverted();
	// calculate world-space bounds using the new matrix
	float3 bmin = bvh->bvhNode[0].aabbMin, bmax = bvh->bvhNode[0].aabbMax;
	bounds = aabb();
	for (int i = 0; i < 8; i++)
		bounds.grow( TransformPosition( float3( i & 1 ? bmax.x : bmin.x,
			i & 2 ? bmax.y : bmin.y, i & 4 ? bmax.z : bmin.z ), transform ) );
}

void BVHInstance::Intersect( Ray& ray )
{
	// backup ray and transform original
	Ray backupRay = ray;
	ray.O = TransformPosition( ray.O, invTransform );
	ray.D = TransformVector( ray.D, invTransform );
	ray.rD = float3( 1 / ray.D.x, 1 / ray.D.y, 1 / ray.D.z );
	// trace ray through BVH
	bvh->Intersect( ray, idx );
	// restore ray origin and direction
	backupRay.hit = ray.hit;
	ray = backupRay;
}

// TLAS implementation

TLAS::TLAS( BVHInstance* bvhList, int N )
{
	// copy a pointer to the array of bottom level accstruc instances
	blas = bvhList;
	blasCount = N;
	// allocate TLAS nodes
	tlasNode = (TLASNode*)_aligned_malloc( sizeof( TLASNode ) * 2 * N + 64, 64 );
	nodeIdx = new uint[N];
	nodesUsed = 2;
}

int TLAS::FindBestMatch( int N, int A )
{
	// find BLAS B that, when joined with A, forms the smallest AABB
	float smallest = 1e30f;
	int bestB = -1;
	for (int B = 0; B < N; B++) if (B != A)
	{
		float3 bmax = fmaxf( tlasNode[nodeIdx[A]].aabbMax, tlasNode[nodeIdx[B]].aabbMax );
		float3 bmin = fminf( tlasNode[nodeIdx[A]].aabbMin, tlasNode[nodeIdx[B]].aabbMin );
		float3 e = bmax - bmin;
		float surfaceArea = e.x * e.y + e.y * e.z + e.z * e.x;
		if (surfaceArea < smallest) smallest = surfaceArea, bestB = B;
	}
	return bestB;
}

void TLAS::Build()
{
	// assign a TLASleaf node to each BLAS
	nodesUsed = 1;
	for (uint i = 0; i < blasCount; i++)
	{
		nodeIdx[i] = nodesUsed;
		tlasNode[nodesUsed].aabbMin = blas[i].bounds.bmin;
		tlasNode[nodesUsed].aabbMax = blas[i].bounds.bmax;
		tlasNode[nodesUsed].BLAS = i;
		tlasNode[nodesUsed++].leftRight = 0; // makes it a leaf
	}
	// use agglomerative clustering to build the TLAS
	int nodeIndices = blasCount;
	int A = 0, B = FindBestMatch( nodeIndices, A );
	FILE* f = fopen( "pairs.txt", "w" );
	while (nodeIndices > 1)
	{
		int C = FindBestMatch( nodeIndices, B );
		if (A == C)
		{
			// found a pair: create a new TLAS interior node
			int nodeIdxA = nodeIdx[A], nodeIdxB = nodeIdx[B];
			TLASNode& nodeA = tlasNode[nodeIdxA];
			TLASNode& nodeB = tlasNode[nodeIdxB];
			TLASNode& newNode = tlasNode[nodesUsed];
			newNode.aabbMin = fminf( nodeA.aabbMin, nodeB.aabbMin );
			newNode.aabbMax = fmaxf( nodeA.aabbMax, nodeB.aabbMax );
			newNode.leftRight = nodeIdxA + (nodeIdxB << 16);
			fprintf( f, "%i,%i\n", nodeIdxA, nodeIdxB );
			nodeIdx[A] = nodesUsed++;
			nodeIdx[B] = nodeIdx[nodeIndices - 1];
			B = FindBestMatch( --nodeIndices, A );
		}
		else A = B, B = C;
	}
	fclose( f );
	// copy last remaining node to the root node
	tlasNode[0] = tlasNode[nodeIdx[A]];
}

struct SortItem { float pos; uint blasIdx; };
static SortItem* item = 0;
static KDTree* tree[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static uint treeSize[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static uint treeIdx = 0;
static void Swap( SortItem& a, SortItem& b ) { SortItem t = a; a = b; b = t; }
int Pivot( SortItem* a, int first, int last )
{
	int p = first;
	SortItem e = a[first];
	for (int i = first + 1; i <= last; i++) if (a[i].pos <= e.pos) Swap( a[i], a[++p] );
	Swap( a[p], a[first] );
	return p;
}
void QuickSort( SortItem a[], int first, int last )
{
	if (first >= last) return;
	int pivotElement = Pivot( a, first, last );
	QuickSort( a, first, pivotElement - 1 );
	QuickSort( a, pivotElement + 1, last );
}

void TLAS::SortAndSplit( uint first, uint last, uint level )
{
	if (!item) item = new SortItem[blasCount];
	uint axis = level % 3; // TODO: use dominant axis at each level?
	if (level == 0) 
	{
		treeIdx = 0;
		for (uint i = 0; i < blasCount; i++) item[i].blasIdx = i;
	}
	for (uint idx, i = first; i <= last; i++)
		idx = item[i].blasIdx,
		item[i].pos = (blas[idx].bounds.bmin[axis] + blas[idx].bounds.bmin[axis]) * 0.5f;
	QuickSort( item, first, last );
	uint half = (first + last) >> 1;
	if (level < 2)
	{
		SortAndSplit( first, half, level + 1 );
		SortAndSplit( half + 1, last, level + 1 );
		return;
	}
	// create chunks
	for( uint i = first; i <= half; i++ )
	{
		BVHInstance& b = blas[item[i].blasIdx];
		tlasNode[nodesUsed].aabbMin = b.bounds.bmin;
		tlasNode[nodesUsed].aabbMax = b.bounds.bmax;
		tlasNode[nodesUsed].BLAS = item[i].blasIdx;
		tlasNode[nodesUsed++].leftRight = 0; // makes it a leaf
	}
	if (!tree[treeIdx]) tree[treeIdx] = new KDTree( tlasNode + first + 32, half - first + 1, first + 32 );
	treeSize[treeIdx++] = half - first + 1;
	for (uint i = half + 1; i <= last; i++)
	{
		BVHInstance& b = blas[item[i].blasIdx];
		tlasNode[nodesUsed].aabbMin = b.bounds.bmin;
		tlasNode[nodesUsed].aabbMax = b.bounds.bmax;
		tlasNode[nodesUsed].BLAS = item[i].blasIdx;
		tlasNode[nodesUsed++].leftRight = 0; // makes it a leaf
	}
	if (!tree[treeIdx]) tree[treeIdx] = new KDTree( tlasNode + half + 33, last - half, half + 33 );
	treeSize[treeIdx++] = last - half;
}

void TLAS::CreateParent( uint idx, uint left, uint right )
{
	tlasNode[idx].left = left, tlasNode[idx].right = right;
	tlasNode[idx].aabbMin = fminf( tlasNode[left].aabbMin, tlasNode[right].aabbMin );
	tlasNode[idx].aabbMax = fmaxf( tlasNode[left].aabbMax, tlasNode[right].aabbMax );
}

void TLAS::BuildQuick()
{
	// single-threaded, reference
#if 0
	// assign a TLASleaf node to each BLAS
	nodesUsed = 1;
	for (uint i = 0; i < blasCount; i++)
	{
		tlasNode[nodesUsed].aabbMin = blas[i].bounds.bmin;
		tlasNode[nodesUsed].aabbMax = blas[i].bounds.bmax;
		tlasNode[nodesUsed].BLAS = i;
		tlasNode[nodesUsed++].leftRight = 0; // makes it a leaf
	}
	// build a kD-tree over the TLAS nodes
	if (!kdtree) kdtree = new KDTree( tlasNode + 1, nodesUsed - 1, 1 /* skip root */ );
	Timer t;
	kdtree->rebuild();
	printf( "kdtree rebuild: %.2fms, ", t.elapsed() * 1000 );
	// use the kD-tree for fast agglomerative clustering
	float sa = 1e30f;
	uint best = 0, workLeft = blasCount, A, B = kdtree->FindNearest( A = 1, best, sa );
	while (1)
	{
		int C = kdtree->FindNearest( B, best = A, sa );
		if (A == C)
		{
			// found a pair: create a new TLAS interior node
			TLASNode& newNode = tlasNode[nodesUsed];
			newNode.aabbMin = fminf( tlasNode[A].aabbMin, tlasNode[B].aabbMin );
			newNode.aabbMax = fmaxf( tlasNode[A].aabbMax, tlasNode[B].aabbMax );
			newNode.leftRight = A + (B << 16);
			if (workLeft-- == 2) break;
			kdtree->removeLeaf( A );
			kdtree->removeLeaf( B );
			kdtree->add( A = nodesUsed++ );
			B = kdtree->FindNearest( A, best = 0, sa = 1e30f );
		}
		else A = B, B = C;
	}
	// copy last remaining node to the root node
	tlasNode[0] = tlasNode[nodesUsed];
#else
	// multi-threaded, using sorted pre-splitting. TODO: generalize to 2^N threads.
	// 1. sort the list of TLAS nodes
	if (!item) item = new SortItem[blasCount];
	uint axis = 0; // TODO: dominant axis
	for (uint i = 0; i < blasCount; i++) item[i].blasIdx = i;
	// 2. split the sorted list into two equally-sized groups
	nodesUsed = 32;
	SortAndSplit( 0, blasCount - 1, 0 );
	// 3. perform agglomerative clustering
	#pragma omp parallel for
	for (int i = 0; i < 8; i++)
	{
		tree[i]->rebuild();
		float sa = 1e30f;
		uint A = 32, B, best = 0, workLeft = treeSize[i], nodePtr = blasCount + 32;
		for( int j = 0; j < i; j++ ) 
			A += treeSize[j], 
			nodePtr += treeSize[j] - 1;
		B = tree[i]->FindNearest( A, best, sa );
		while (1)
		{
			int C = tree[i]->FindNearest( B, best = A, sa );
			if (A == C)
			{
				// found a pair: create a new TLAS interior node
				TLASNode& newNode = tlasNode[nodePtr];
				newNode.aabbMin = fminf( tlasNode[A].aabbMin, tlasNode[B].aabbMin );
				newNode.aabbMax = fmaxf( tlasNode[A].aabbMax, tlasNode[B].aabbMax );
				newNode.leftRight = A + (B << 16);
				if (workLeft-- == 2) break;
				tree[i]->removeLeaf( A );
				tree[i]->removeLeaf( B );
				tree[i]->add( A = nodePtr++ );
				B = tree[i]->FindNearest( A, best = 0, sa = 1e30f );
			}
			else A = B, B = C;
		}
		// copy last remaining node to the root node
		tlasNode[i + 7] = tlasNode[nodePtr];
	}
	// 4. join together the resulting trees
	CreateParent( 3, 7, 8 );
	CreateParent( 4, 9, 10 );
	CreateParent( 5, 11, 12 );
	CreateParent( 6, 13, 14 );
	CreateParent( 1, 3, 4 );
	CreateParent( 2, 5, 6 );
	CreateParent( 0, 1, 2 );
	// 5. profit.
	nodesUsed = 2 * blasCount + 64;
#endif
}

void TLAS::Intersect( Ray& ray )
{
	// calculate reciprocal ray directions for faster AABB intersection
	ray.rD = float3( 1 / ray.D.x, 1 / ray.D.y, 1 / ray.D.z );
	// use a local stack instead of a recursive function
	TLASNode* node = &tlasNode[0], * stack[64];
	uint stackPtr = 0;
	// traversl loop; terminates when the stack is empty
	while (1)
	{
		if (node->isLeaf())
		{
			// current node is a leaf: intersect BLAS
			blas[node->BLAS].Intersect( ray );
			// pop a node from the stack; terminate if none left
			if (stackPtr == 0) break; else node = stack[--stackPtr];
			continue;
		}
		// current node is an interior node: visit child nodes, ordered
		TLASNode* child1 = &tlasNode[node->leftRight & 0xffff];
		TLASNode* child2 = &tlasNode[node->leftRight >> 16];
		float dist1 = IntersectAABB( ray, child1->aabbMin, child1->aabbMax );
		float dist2 = IntersectAABB( ray, child2->aabbMin, child2->aabbMax );
		if (dist1 > dist2) { swap( dist1, dist2 ); swap( child1, child2 ); }
		if (dist1 == 1e30f)
		{
			// missed both child nodes; pop a node from the stack
			if (stackPtr == 0) break; else node = stack[--stackPtr];
		}
		else
		{
			// visit near node; push the far node if the ray intersects it
			node = child1;
			if (dist2 != 1e30f) stack[stackPtr++] = child2;
		}
	}
}

// EOF