
#include <assert.h>

class hddlBitstreamBase: public mfxBitstream
{
public:
	hddlBitstreamBase(const int buffMaxLen = 1024*1024){
	    memset((mfxBitstream*)this, 0, sizeof(mfxBitstream));
	    this->mfxBitstream::MaxLength = buffMaxLen;
	    this->mfxBitstream::Data = new mfxU8[this->mfxBitstream::MaxLength];
	    assert(this->mfxBitstream::Data);
	}
	virtual ~hddlBitstreamBase(){
		if(this->mfxBitstream::Data)
			delete []this->mfxBitstream::Data;
	}
	virtual mfxU32 Feed(void)=0;
	virtual bool IsEnd(void)=0;
};

class hddlBitstreamFile: public hddlBitstreamBase
{
public:
	hddlBitstreamFile(const char * fname, bool bRepeat = false){
		m_fSource = fopen(fname,"rb");
		m_bRepeat = bRepeat;
		assert(m_fSource);
	}
	virtual ~hddlBitstreamFile(){
		if(m_fSource) fclose(m_fSource);
	}

	virtual mfxU32 Feed(void){

		memmove(this->Data, this->Data + this->DataOffset, this->DataLength);
		this->DataOffset = 0;

		mfxU32 nBytesRead = 0;
		mfxU32 nBytesSpace = this->MaxLength - this->DataLength;
		while(!m_bEOS && nBytesRead == 0 && nBytesSpace > 0)
		{
			nBytesRead = (mfxU32) fread(this->Data + this->DataLength, 1, nBytesSpace, m_fSource);

			//printf("nBytesSpace=%d, nBytesRead=%d\n", nBytesSpace, nBytesRead);

			if (0 == nBytesRead){
				if(m_bRepeat)
					fseek(m_fSource, 0, SEEK_SET);
				else
					m_bEOS = true;
			}
		}
		this->TimeStamp ++;
		this->DataLength += nBytesRead;
		return nBytesRead;
	}
	virtual bool IsEnd(void){
		return m_bEOS && this->DataLength == 0;
	}

	FILE *m_fSource = NULL;
	bool m_bEOS = false;
	bool m_bRepeat = false;
};
