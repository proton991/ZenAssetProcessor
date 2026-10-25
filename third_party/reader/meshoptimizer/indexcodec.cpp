// Generated from pinned meshoptimizer v1.1; see cmake/MeshoptDecoder.cmake.
// This file is part of meshoptimizer library; see meshoptimizer.h for version/license details
#include "Codecs/MeshoptDecoder.h"

#include <assert.h>
#include <string.h>

// This work is based on:
// Fabian Giesen. Simple lossless index buffer compression & follow-up. 2013
// Conor Stokes. Vertex Cache Optimised Index Buffer Compression. 2014
namespace zenasset_meshopt_detail
{

const unsigned char kIndexHeader = 0xe0;
const unsigned char kSequenceHeader = 0xd0;

const int kDecodeIndexVersion = 1;

typedef unsigned int VertexFifo[16];
typedef unsigned int EdgeFifo[16][2];

static void pushEdgeFifo(EdgeFifo fifo, unsigned int a, unsigned int b, size_t& offset)
{
	fifo[offset][0] = a;
	fifo[offset][1] = b;
	offset = (offset + 1) & 15;
}

static void pushVertexFifo(VertexFifo fifo, unsigned int v, size_t& offset, int cond = 1)
{
	fifo[offset] = v;
	offset = (offset + cond) & 15;
}

static unsigned int decodeVByte(const unsigned char*& data)
{
	unsigned char lead = *data++;

	// fast path: single byte
	if (lead < 128)
		return lead;

	// slow path: up to 4 extra bytes
	// note that this loop always terminates, which is important for malformed data
	unsigned int result = lead & 127;
	unsigned int shift = 7;

	for (int i = 0; i < 4; ++i)
	{
		unsigned char group = *data++;
		result |= unsigned(group & 127) << shift;
		shift += 7;

		if (group < 128)
			break;
	}

	return result;
}

static unsigned int decodeIndex(const unsigned char*& data, unsigned int last)
{
	unsigned int v = decodeVByte(data);
	unsigned int d = (v >> 1) ^ -int(v & 1);

	return last + d;
}

static void writeTriangle(void* destination, size_t offset, size_t index_size, unsigned int a, unsigned int b, unsigned int c)
{
	if (index_size == 2)
	{
		static_cast<unsigned short*>(destination)[offset + 0] = (unsigned short)(a);
		static_cast<unsigned short*>(destination)[offset + 1] = (unsigned short)(b);
		static_cast<unsigned short*>(destination)[offset + 2] = (unsigned short)(c);
	}
	else
	{
		static_cast<unsigned int*>(destination)[offset + 0] = a;
		static_cast<unsigned int*>(destination)[offset + 1] = b;
		static_cast<unsigned int*>(destination)[offset + 2] = c;
	}
}

} // namespace zenasset_meshopt_detail

int zenasset_meshopt_decodeIndexVersion(const unsigned char* buffer, size_t buffer_size)
{
	if (buffer_size < 1)
		return -1;

	unsigned char header = buffer[0];

	if ((header & 0xf0) != zenasset_meshopt_detail::kIndexHeader && (header & 0xf0) != zenasset_meshopt_detail::kSequenceHeader)
		return -1;

	int version = header & 0x0f;
	if (version > zenasset_meshopt_detail::kDecodeIndexVersion)
		return -1;

	return version;
}

int zenasset_meshopt_decodeIndexBuffer(void* destination, size_t index_count, size_t index_size, const unsigned char* buffer, size_t buffer_size)
{
	using namespace zenasset_meshopt_detail;

	assert(index_count % 3 == 0);
	assert(index_size == 2 || index_size == 4);

	// the minimum valid encoding is header, 1 byte per triangle and a 16-byte codeaux table
	if (buffer_size < 1 + index_count / 3 + 16)
		return -2;

	if ((buffer[0] & 0xf0) != kIndexHeader)
		return -1;

	int version = buffer[0] & 0x0f;
	if (version > kDecodeIndexVersion)
		return -1;

	EdgeFifo edgefifo;
	memset(edgefifo, -1, sizeof(edgefifo));

	VertexFifo vertexfifo;
	memset(vertexfifo, -1, sizeof(vertexfifo));

	size_t edgefifooffset = 0;
	size_t vertexfifooffset = 0;

	unsigned int next = 0;
	unsigned int last = 0;

	int fecmax = version >= 1 ? 13 : 15;

	// since we store 16-byte codeaux table at the end, triangle data has to begin before data_safe_end
	const unsigned char* code = buffer + 1;
	const unsigned char* data = code + index_count / 3;
	const unsigned char* data_safe_end = buffer + buffer_size - 16;

	const unsigned char* codeaux_table = data_safe_end;

	for (size_t i = 0; i < index_count; i += 3)
	{
		// make sure we have enough data to read for a triangle
		// each triangle reads at most 16 bytes of data: 1b for codeaux and 5b for each free index
		// after this we can be sure we can read without extra bounds checks
		if (data > data_safe_end)
			return -2;

		unsigned char codetri = *code++;

		if (codetri < 0xf0)
		{
			int fe = codetri >> 4;

			// fifo reads are wrapped around 16 entry buffer
			unsigned int a = edgefifo[(edgefifooffset - 1 - fe) & 15][0];
			unsigned int b = edgefifo[(edgefifooffset - 1 - fe) & 15][1];
			unsigned int c = 0;

			int fec = codetri & 15;

			// note: this is the most common path in the entire decoder
			// inside this if we try to stay branchless (by using cmov/etc.) since these aren't predictable
			if (fec < fecmax)
			{
				// fifo reads are wrapped around 16 entry buffer
				unsigned int cf = vertexfifo[(vertexfifooffset - 1 - fec) & 15];
				c = (fec == 0) ? next : cf;

				int fec0 = fec == 0;
				next += fec0;

				// push vertex fifo must match the encoding step *exactly* otherwise the data will not be decoded correctly
				pushVertexFifo(vertexfifo, c, vertexfifooffset, fec0);
			}
			else
			{
				// fec - (fec ^ 3) decodes 13, 14 into -1, 1
				// note that we need to update the last index since free indices are delta-encoded
				last = c = (fec != 15) ? last + (fec - (fec ^ 3)) : decodeIndex(data, last);

				// push vertex/edge fifo must match the encoding step *exactly* otherwise the data will not be decoded correctly
				pushVertexFifo(vertexfifo, c, vertexfifooffset);
			}

			// push edge fifo must match the encoding step *exactly* otherwise the data will not be decoded correctly
			pushEdgeFifo(edgefifo, c, b, edgefifooffset);
			pushEdgeFifo(edgefifo, a, c, edgefifooffset);

			// output triangle
			writeTriangle(destination, i, index_size, a, b, c);
		}
		else
		{
			// fast path: read codeaux from the table
			if (codetri < 0xfe)
			{
				unsigned char codeaux = codeaux_table[codetri & 15];

				// note: table can't contain feb/fec=15
				int feb = codeaux >> 4;
				int fec = codeaux & 15;

				// fifo reads are wrapped around 16 entry buffer
				// also note that we increment next for all three vertices before decoding indices - this matches encoder behavior
				unsigned int a = next++;

				unsigned int bf = vertexfifo[(vertexfifooffset - feb) & 15];
				unsigned int b = (feb == 0) ? next : bf;

				int feb0 = feb == 0;
				next += feb0;

				unsigned int cf = vertexfifo[(vertexfifooffset - fec) & 15];
				unsigned int c = (fec == 0) ? next : cf;

				int fec0 = fec == 0;
				next += fec0;

				// output triangle
				writeTriangle(destination, i, index_size, a, b, c);

				// push vertex/edge fifo must match the encoding step *exactly* otherwise the data will not be decoded correctly
				pushVertexFifo(vertexfifo, a, vertexfifooffset);
				pushVertexFifo(vertexfifo, b, vertexfifooffset, feb0);
				pushVertexFifo(vertexfifo, c, vertexfifooffset, fec0);

				pushEdgeFifo(edgefifo, b, a, edgefifooffset);
				pushEdgeFifo(edgefifo, c, b, edgefifooffset);
				pushEdgeFifo(edgefifo, a, c, edgefifooffset);
			}
			else
			{
				// slow path: read a full byte for codeaux instead of using a table lookup
				unsigned char codeaux = *data++;

				int fea = codetri == 0xfe ? 0 : 15;
				int feb = codeaux >> 4;
				int fec = codeaux & 15;

				// reset: codeaux is 0 but encoded as not-a-table
				if (codeaux == 0)
					next = 0;

				// fifo reads are wrapped around 16 entry buffer
				// also note that we increment next for all three vertices before decoding indices - this matches encoder behavior
				unsigned int a = (fea == 0) ? next++ : 0;
				unsigned int b = (feb == 0) ? next++ : vertexfifo[(vertexfifooffset - feb) & 15];
				unsigned int c = (fec == 0) ? next++ : vertexfifo[(vertexfifooffset - fec) & 15];

				// note that we need to update the last index since free indices are delta-encoded
				if (fea == 15)
					last = a = decodeIndex(data, last);

				if (feb == 15)
					last = b = decodeIndex(data, last);

				if (fec == 15)
					last = c = decodeIndex(data, last);

				// output triangle
				writeTriangle(destination, i, index_size, a, b, c);

				// push vertex/edge fifo must match the encoding step *exactly* otherwise the data will not be decoded correctly
				pushVertexFifo(vertexfifo, a, vertexfifooffset);
				pushVertexFifo(vertexfifo, b, vertexfifooffset, (feb == 0) | (feb == 15));
				pushVertexFifo(vertexfifo, c, vertexfifooffset, (fec == 0) | (fec == 15));

				pushEdgeFifo(edgefifo, b, a, edgefifooffset);
				pushEdgeFifo(edgefifo, c, b, edgefifooffset);
				pushEdgeFifo(edgefifo, a, c, edgefifooffset);
			}
		}
	}

	// we should've read all data bytes and stopped at the boundary between data and codeaux table
	if (data != data_safe_end)
		return -3;

	return 0;
}

int zenasset_meshopt_decodeIndexSequence(void* destination, size_t index_count, size_t index_size, const unsigned char* buffer, size_t buffer_size)
{
	using namespace zenasset_meshopt_detail;

	// the minimum valid encoding is header, 1 byte per index and a 4-byte tail
	if (buffer_size < 1 + index_count + 4)
		return -2;

	if ((buffer[0] & 0xf0) != kSequenceHeader)
		return -1;

	int version = buffer[0] & 0x0f;
	if (version > kDecodeIndexVersion)
		return -1;

	const unsigned char* data = buffer + 1;
	const unsigned char* data_safe_end = buffer + buffer_size - 4;

	unsigned int last[2] = {};

	for (size_t i = 0; i < index_count; ++i)
	{
		// make sure we have enough data to read
		// each index reads at most 5 bytes of data; there's a 4 byte tail after data_safe_end
		// after this we can be sure we can read without extra bounds checks
		if (data >= data_safe_end)
			return -2;

		unsigned int v = decodeVByte(data);

		// decode the index of the last baseline
		unsigned int current = v & 1;
		v >>= 1;

		// reconstruct index as a delta
		unsigned int d = (v >> 1) ^ -int(v & 1);
		unsigned int index = last[current] + d;

		// update last for the next iteration that uses it
		last[current] = index;

		if (index_size == 2)
		{
			static_cast<unsigned short*>(destination)[i] = (unsigned short)(index);
		}
		else
		{
			static_cast<unsigned int*>(destination)[i] = index;
		}
	}

	// we should've read all data bytes and stopped at the boundary between data and tail
	if (data != data_safe_end)
		return -3;

	return 0;
}
