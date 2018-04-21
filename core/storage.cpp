#include "storage.h"
#include "ecc_native.h"

namespace beam {

/////////////////////////////
// RadixTree
uint16_t RadixTree::Node::get_Bits() const
{
	return m_Bits & ~(s_Clean | s_Leaf);
}

const uint8_t* RadixTree::Node::get_Key() const
{
	return (s_Leaf & m_Bits) ? ((Leaf*) this)->m_pKeyArr : ((Joint*) this)->m_pKeyPtr;
}

RadixTree::RadixTree()
	:m_pRoot(NULL)
{
}

RadixTree::~RadixTree()
{
	assert(!m_pRoot);
}

void RadixTree::Clear()
{
	if (m_pRoot)
	{
		DeleteNode(m_pRoot);
		m_pRoot = NULL;
	}
}

void RadixTree::DeleteNode(Node* p)
{
	if (Node::s_Leaf & p->m_Bits)
		DeleteLeaf((Leaf*) p);
	else
	{
		Joint* p1 = (Joint*) p;

		for (int i = 0; i < _countof(p1->m_ppC); i++)
			DeleteNode(p1->m_ppC[i]);

		DeleteJoint(p1);
	}
}

uint8_t RadixTree::CursorBase::get_BitRaw(const uint8_t* p0) const
{
	return p0[m_nBits >> 3] >> (7 ^ (7 & m_nBits));
}

uint8_t RadixTree::CursorBase::get_Bit(const uint8_t* p0) const
{
	return 1 & get_BitRaw(p0);
}

RadixTree::Leaf& RadixTree::CursorBase::get_Leaf() const
{
	assert(m_nPtrs);
	Leaf* p = (Leaf*) m_pp[m_nPtrs - 1];
	assert(Node::s_Leaf & p->m_Bits);
	return *p;
}

void RadixTree::CursorBase::Invalidate()
{
	for (uint32_t n = m_nPtrs; n--; )
	{
		Node* p = m_pp[n];
		assert(p);

		if (!(Node::s_Clean & p->m_Bits))
			break;

		p->m_Bits &= ~Node::s_Clean;
	}
}

void RadixTree::ReplaceTip(CursorBase& cu, Node* pNew)
{
	assert(cu.m_nPtrs);
	Node* pOld = cu.m_pp[cu.m_nPtrs - 1];
	assert(pOld);

	if (cu.m_nPtrs > 1)
	{
		Joint* pPrev = (Joint*) cu.m_pp[cu.m_nPtrs - 2];
		assert(pPrev);

		for (int i = 0; ; i++)
		{
			assert(i < _countof(pPrev->m_ppC));
			if (pPrev->m_ppC[i] == pOld)
			{
				pPrev->m_ppC[i] = pNew;
				break;
			}
		}
	} else
	{
		assert(m_pRoot == pOld);
		m_pRoot = pNew;
	}
}

bool RadixTree::Goto(CursorBase& cu, const uint8_t* pKey, uint32_t nBits) const
{
	Node* p = m_pRoot;

	if (p)
	{
		cu.m_pp[0] = p;
		cu.m_nPtrs = 1;
	} else
		cu.m_nPtrs = 0;

	cu.m_nBits = 0;
	cu.m_nPosInLastNode = 0;

	while (nBits > cu.m_nBits)
	{
		if (!p)
			return false;

		const uint8_t* pKeyNode = p->get_Key();

		uint32_t nThreshold = std::min(cu.m_nBits + p->get_Bits(), nBits);

		for ( ; cu.m_nBits < nThreshold; cu.m_nBits++, cu.m_nPosInLastNode++)
			if (1 & (cu.get_BitRaw(pKey) ^ cu.get_BitRaw(pKeyNode)))
				return false; // no match

		if (cu.m_nBits == nBits)
			return true;

		assert(cu.m_nPosInLastNode == p->get_Bits());

		Joint* pN = (Joint*) p;
		p = pN->m_ppC[cu.get_Bit(pKey)];

		assert(p); // joints should have both children!

		cu.m_pp[cu.m_nPtrs++] = p;
		cu.m_nBits++;
		cu.m_nPosInLastNode = 0;
	}

	return true;
}

RadixTree::Leaf* RadixTree::Find(CursorBase& cu, const uint8_t* pKey, uint32_t nBits, bool& bCreate)
{
	if (Goto(cu, pKey, nBits))
	{
		bCreate = false;
		return &cu.get_Leaf();
	}

	assert(cu.m_nBits < nBits);

	if (!bCreate)
		return NULL;

	Leaf* pN = CreateLeaf();

	// Guard the allocated leaf. In case exc will be thrown (during possible allocation of a new joint)
	struct Guard
	{
		Leaf* m_pLeaf;
		RadixTree* m_pTree;

		~Guard() {
			if (m_pLeaf)
				m_pTree->DeleteLeaf(m_pLeaf);
		}
	} g;

	g.m_pTree = this;
	g.m_pLeaf = pN;


	memcpy(pN->m_pKeyArr, pKey, (nBits + 7) >> 3);

	if (cu.m_nPtrs)
	{
		cu.Invalidate();

		uint32_t iC = cu.get_Bit(pKey);

		Node* p = cu.m_pp[cu.m_nPtrs - 1];
		assert(p);

		const uint8_t* pKey1 = p->get_Key();
		assert(cu.get_Bit(pKey1) != iC);

		// split
		Joint* pJ = CreateJoint();
		pJ->m_pKeyPtr = /*pN->m_pKeyArr*/pKey1;
		pJ->m_Bits = cu.m_nPosInLastNode;

		ReplaceTip(cu, pJ);
		cu.m_pp[cu.m_nPtrs - 1] = pJ;

		pN->m_Bits = nBits - (cu.m_nBits + 1);
		p->m_Bits -= cu.m_nPosInLastNode + 1;

		pJ->m_ppC[iC] = pN;
		pJ->m_ppC[!iC] = p;


	} else
	{
		assert(!m_pRoot);
		m_pRoot = pN;
		pN->m_Bits = nBits;
	}

	cu.m_pp[cu.m_nPtrs++] = pN;
	cu.m_nPosInLastNode = pN->m_Bits; // though not really necessary
	cu.m_nBits = nBits;

	pN->m_Bits |= Node::s_Leaf;

	g.m_pLeaf = NULL; // dismissed

	return pN;
}

void RadixTree::Delete(CursorBase& cu)
{
	assert(cu.m_nPtrs);

	cu.Invalidate();

	Leaf* p = (Leaf*) cu.m_pp[cu.m_nPtrs - 1];
	assert(Node::s_Leaf & p->m_Bits);

	const uint8_t* pKeyDead = p->m_pKeyArr;

	ReplaceTip(cu, NULL);
	DeleteLeaf(p);

	if (1 == cu.m_nPtrs)
		assert(!m_pRoot);
	else
	{
		cu.m_nPtrs--;

		Joint* pPrev = (Joint*) cu.m_pp[cu.m_nPtrs - 1];
		for (int i = 0; ; i++)
		{
			assert(i < _countof(pPrev->m_ppC));
			Node* p = pPrev->m_ppC[i];
			if (p)
			{
				const uint8_t* pKey1 = (p->m_Bits & Node::s_Leaf) ? ((Leaf*) p)->m_pKeyArr : ((Joint*) p)->m_pKeyPtr;
				assert(pKey1 != pKeyDead);

				for (uint32_t j = cu.m_nPtrs; j--; )
				{
					Joint* pPrev2 = (Joint*) cu.m_pp[j];
					if (pPrev2->m_pKeyPtr != pKeyDead)
						break;

					pPrev2->m_pKeyPtr = pKey1;
				}

				p->m_Bits += pPrev->m_Bits + 1;
				ReplaceTip(cu, p);

				DeleteJoint(pPrev);

				break;
			}
		}
	}
}

bool RadixTree::Traverse(const Node& n, ITraveler& t)
{
	if (Node::s_Leaf & n.m_Bits)
		return t.OnLeaf((const Leaf&) n);

	const Joint& x = (const Joint&) n;
	for (int i = 0; i < _countof(x.m_ppC); i++)
		if (!Traverse(*x.m_ppC[i], t))
			return false;

	return true;
}

bool RadixTree::Traverse(ITraveler& t) const
{
	return m_pRoot ? Traverse(*m_pRoot, t) : false;
}

bool RadixTree::Traverse(const CursorBase& cu, ITraveler& t)
{
	return cu.m_nPtrs ? Traverse(*cu.m_pp[cu.m_nPtrs - 1], t) : true;
}

size_t RadixTree::Count() const
{
	struct Traveler
		:public ITraveler
	{
		size_t m_Count;
		virtual bool OnLeaf(const Leaf&) override {
			m_Count++;
			return true;
		}
	} t;

	t.m_Count = 0;
	Traverse(t);
	return t.m_Count;
}

/////////////////////////////
// UtxoTree
UtxoTree::UtxoTree()
{
	MyLeaf x;
	RadixTree::Leaf& y = x;
	printf("UtxoTree::MyLeaf=%p, Buf=%p, RadixTree::Leaf=%p, Key=%p\n", &x, x.m_pPlaceholder, &y, y.m_pKeyArr);

	{
		UtxoTree::Cursor x;
		RadixTree::CursorBase& y = x;
		printf("UtxoTree::Cursor=%p, Buf=%p, RadixTree::Leaf=%p, Key=%p\n", &x, x.get_ppExtra(), &y, y.get_pp());
	}
	fflush(stdout);
}

void UtxoTree::get_Hash(Merkle::Hash& hv)
{
	Node* p = get_Root();
	if (p)
		hv = get_Hash(*p, hv);
	else
		hv = ECC::Zero;
}

void UtxoTree::Value::get_Hash(Merkle::Hash& hv, const Key& key) const
{
	ECC::Hash::Processor hp;
	hp.Write(key.m_pArr, Key::s_Bytes); // whole description of the UTXO
	hp << m_Count;

	hp >> hv;
}

const Merkle::Hash& UtxoTree::get_Hash(Node& n, Merkle::Hash& hv)
{
	if (Node::s_Leaf & n.m_Bits)
	{
		MyLeaf& x = (MyLeaf&) n;
		x.m_Bits |= Node::s_Clean;

		x.m_Value.get_Hash(hv, x.get_Key());

		return hv;

	}

	MyJoint& x = (MyJoint&) n;
	if (!(Node::s_Clean & x.m_Bits))
	{
		ECC::Hash::Processor hp;

		for (int i = 0; i < _countof(x.m_ppC); i++)
		{
			ECC::Hash::Value hv;
			hp << get_Hash(*x.m_ppC[i], hv);
		}

		hp >> x.m_Hash;
		x.m_Bits |= Node::s_Clean;
	}

	return x.m_Hash;
}

void UtxoTree::Cursor::get_Proof(Merkle::Proof& proof) const
{
	uint32_t n = m_nPtrs;
	assert(n);

	for (const Node* pPrev = m_pp[--n]; n--; )
	{
		const Joint& x = (const Joint&) *m_pp[n];

		Merkle::Node node;
		node.first = (x.m_ppC[0] == pPrev);

		node.second = get_Hash(*x.m_ppC[node.first != false], node.second);

		proof.push_back(std::move(node));
		pPrev = &x;
	}
}

void UtxoTree::SaveIntenral(ISerializer& s) const
{
	uint32_t n = (uint32_t) Count();
	s.Process(n);

	struct Traveler
		:public ITraveler
	{
		ISerializer* m_pS;
		virtual bool OnLeaf(const Leaf& n) override {
			MyLeaf& x = (MyLeaf&) n;
			m_pS->Process(x.get_Key());
			m_pS->Process(x.m_Value);
			return true;
		}
	} t;
	t.m_pS = &s;
	Traverse(t);
}

void UtxoTree::LoadIntenral(ISerializer& s)
{
	Clear();

	uint32_t n = 0;
	s.Process(n);

	Key pKey[2];

	for (uint32_t i = 0; i < n; i++)
	{
		Key& key = pKey[1 & i];
		const Key& keyPrev = pKey[!(1 & i)];

		s.Process(key);

		if (i)
		{
			// must be in ascending order
			if (keyPrev.cmp(key) >= 0)
				throw std::runtime_error("incorrect order");
		}

		Cursor cu;
		bool bCreate = true;
		MyLeaf* p = Find(cu, key, bCreate);

		p->m_Value.m_Count = 0;
		s.Process(p->m_Value);
	}
}

int UtxoTree::Key::cmp(const Key& k) const
{
	return memcmp(m_pArr, k.m_pArr, sizeof(m_pArr));
}

UtxoTree::Key::Formatted& UtxoTree::Key::Formatted::operator = (const Key& key)
{
	memcpy(m_Commitment.m_X.m_pData, key.m_pArr, sizeof(m_Commitment.m_X.m_pData));
	const uint8_t* pKey = key.m_pArr + sizeof(m_Commitment.m_X.m_pData);

	m_Commitment.m_Y	= (1 & (pKey[0] >> 7)) != 0;
	m_bCoinbase			= (1 & (pKey[0] >> 6)) != 0;
	m_bConfidential		= (1 & (pKey[0] >> 5)) != 0;

	m_Height = 0;
	for (int i = 0; i < sizeof(m_Height); i++, pKey++)
		m_Height = (m_Height << 8) | (pKey[0] << 3) | (pKey[1] >> 5);

	return *this;
}

UtxoTree::Key& UtxoTree::Key::operator = (const Key::Formatted& fmt)
{
	memcpy(m_pArr, fmt.m_Commitment.m_X.m_pData, sizeof(fmt.m_Commitment.m_X.m_pData));

	uint8_t* pKey = m_pArr + sizeof(fmt.m_Commitment.m_X.m_pData);
	memset(pKey, 0, sizeof(m_pArr) - sizeof(fmt.m_Commitment.m_X.m_pData));

	if (fmt.m_Commitment.m_Y)
		pKey[0] |= (1 << 7);
	if (fmt.m_bCoinbase)
		pKey[0] |= (1 << 6);
	if (fmt.m_bConfidential)
		pKey[0] |= (1 << 5);

	for (int i = 0; i < sizeof(fmt.m_Height); i++)
	{
		uint8_t val = uint8_t(fmt.m_Height >> ((sizeof(fmt.m_Height) - i - 1) << 3));
		pKey[i] |= val >> 3;
		pKey[i + 1] |= (val << 5);
	}

	return *this;
}

UtxoTree::Key& UtxoTree::MyLeaf::get_Key() const
{
	return (Key&) m_pKeyArr; // should be fine
}

/////////////////////////////
// Merkle::Mmr
void Merkle::Mmr::Append(const Merkle::Hash& hv)
{
	Merkle::Hash hv1 = hv;
	uint32_t n = m_Count;

	for (uint32_t nHeight = 0; ; nHeight++, n >>= 1)
	{
		SaveElement(hv1, n, nHeight);
		if (!(1 & n))
			break;

		Merkle::Hash hv0;
		LoadElement(hv0, n ^ 1, nHeight);

		ECC::Hash::Processor() << hv0 << hv1 >> hv1;
	}

	m_Count++;
}

void Merkle::Mmr::get_Hash(Merkle::Hash& hv) const
{
	if (!get_HashForRange(hv, 0, m_Count))
		hv = ECC::Zero;
}

bool Merkle::Mmr::get_HashForRange(Merkle::Hash& hv, uint32_t n0, uint32_t n) const
{
	bool bEmpty = true;

	for (uint32_t nHeight = 0; n; nHeight++, n >>= 1, n0 >>= 1)
		if (1 & n)
		{
			Merkle::Hash hv0;
			LoadElement(hv0, n0 + n ^ 1, nHeight);

			if (bEmpty)
			{
				hv = hv0;
				bEmpty = false;
			}
			else
				ECC::Hash::Processor() << hv0 << hv >> hv;
		}

	return !bEmpty;
}

void Merkle::Mmr::get_Proof(Proof& proof, uint32_t i) const
{
	assert(i < m_Count);

	uint32_t n = m_Count;
	for (uint32_t nHeight = 0; n; nHeight++, n >>= 1, i >>= 1)
	{
		Merkle::Node node;
		node.first = !(i & 1);

		uint32_t nSibling = i ^ 1;
		bool bFullSibling = !node.first;

		if (!bFullSibling)
		{
			uint32_t n0 = nSibling << nHeight;
			if (n0 >= m_Count)
				continue;

			uint32_t nRemaining = m_Count - n0;
			if (nRemaining >> nHeight)
				bFullSibling = true;
			else
				verify(get_HashForRange(node.second, n0, nRemaining));
		}

		if (bFullSibling)
			LoadElement(node.second, nSibling, nHeight);

		proof.push_back(std::move(node));
	}
}



} // namespace beam
