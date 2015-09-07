#include "MinHashEncoder.h"

MinHashEncoder::~MinHashEncoder(){

}

MinHashEncoder::MinHashEncoder(Parameters* apParameters, Data* apData, INDEXType apIndexType)
{
	Init(apParameters, apData, apIndexType);
}

void MinHashEncoder::Init(Parameters* apParameters, Data* apData, INDEXType apIndexType) {
	mpParameters = apParameters;
	mpData = apData;
	numKeys=0;
	numFullBins=0;
	mHashBitMask = numeric_limits<unsigned>::max() >> 1;
	mHashBitMask = (2 << (mpParameters->mHashBitSize - 1)) - 1;
	indexType = apIndexType;

	if (mpParameters->mNumRepeatsHashFunction == 0 || mpParameters->mNumRepeatsHashFunction > mpParameters->mNumHashShingles * mpParameters->mNumHashFunctions){
		mpParameters->mNumRepeatsHashFunction = mpParameters->mNumHashShingles * mpParameters->mNumHashFunctions;
	}

	if (mpParameters->mSeqWindow != 0 && mpParameters->mSeqShift == 0)

		cout << "MinHashEncoder object created of indexType " << indexType << endl;
}

inline vector<unsigned> MinHashEncoder::HashFuncNSPDK(const string& aString, unsigned aStart, unsigned aMaxRadius, unsigned aBitMask) {
	unsigned int hash = 0xAAAAAAAA;
	unsigned effective_end = min((unsigned) aString.size() - 1, aStart + aMaxRadius);
	unsigned radius = 0;
	vector<unsigned> code_list(aMaxRadius + 1, 0);
	for (std::size_t i = aStart; i <= effective_end; i++) {
		hash ^= ((i & 1) == 0) ? ((hash << 7) ^ aString[i] * (hash >> 3)) : (~(((hash << 11) + aString[i]) ^ (hash >> 5)));
		code_list[radius] = hash & aBitMask;
		radius++;
	}
	return code_list;
}

void MinHashEncoder::generate_feature_vector(const GraphClass& aG, SVector& x) {
	//assume there is a mMinRadius and a mMinDistance
	unsigned mRadius = mpParameters->mRadius;
	unsigned mDistance = mpParameters->mDistance;
	unsigned mMinRadius = mpParameters->mMinRadius;
	unsigned mMinDistance = mpParameters->mMinDistance;

	//assume 1 vertex with all info on the label
	string seq = aG.GetVertexLabel(0);
	unsigned size = seq.size();

	vector<vector<unsigned> > mFeatureCache;
	for (unsigned i = 0; i < size; ++i) {
		mFeatureCache.push_back(vector<unsigned>(mRadius, 0));
	}

	//create neighborhood features
	for (unsigned start = 0; start < size; ++start)
		mFeatureCache[start] = HashFuncNSPDK(seq, start, mRadius, mHashBitMask);

	vector<unsigned> endpoint_list(4);
	for (unsigned r = mMinRadius; r <= mRadius; r++) {
		endpoint_list[0] = r;
		for (unsigned d = mMinDistance; d <= mDistance; d++) {
			endpoint_list[1] = d;
			SVector z(pow(2, mpParameters->mHashBitSize));
			for (unsigned start = 0; start < size; ++start) {
				unsigned src_code = mFeatureCache[start][r];
				unsigned effective_dest = min(start + d, size - 1);
				unsigned dest_code = mFeatureCache[effective_dest][r];
				if (src_code > dest_code) {
					endpoint_list[2] = src_code;
					endpoint_list[3] = dest_code;
				} else {
					endpoint_list[3] = src_code;
					endpoint_list[2] = dest_code;
				}
				unsigned code = HashFunc(endpoint_list, mHashBitMask);
				//				z.coeffRef(code) += 1;
				//
				//				//add feature that ignore src endpoint
				//				//NOTE: this is important when the label of src vertex is considered noisy, in this way, it is only the context that will define the features
				//				endpoint_list[2] = 0;
				//				endpoint_list[3] = dest_code;
				//				unsigned nosrc_code = HashFunc(endpoint_list, mHashBitMask);
				//				z.coeffRef(nosrc_code) += 1;
				x.coeffRef(code) = 1;
			}
			z /= z.norm();
			x += z;
		}
	}
	x /= x.norm();
}

void MinHashEncoder::worker_readFiles(int numWorkers){

	int file_instances = 0;
	std::tr1::unordered_map<string, uint8_t> seq_names_seen;

	while (!done){

		SeqFileP myData;
		bool succ = readFile_queue.try_pop(myData);

		if (!done && succ && myData->filename != ""){

			cout << endl << "read next file " << myData->filename << " sig_all_counter " << mSignatureCounter << " inst_counter "<< mInstanceCounter << " thread " << std::this_thread::get_id() << endl;
			igzstream fin;
			fin.open(myData->filename.c_str(),std::ios::in);

			if (!fin)
				throw range_error("ERROR Data::LoadData: Cannot open file: " + myData->filename);

			file_instances = 0; // only for log output

			unsigned pos = 0; // tracks the current seq start pos (window/shift)
			unsigned end = 0; // tracks the current seq end pos, set from BED entry or to full seq end
			unsigned idx = 0; // holds the instance id for the inverse index

			bool valid_input = false; // set to false so that we get new seq in while further down directly
			string currSeq;
			string currFullSeq;
			string currSeqName;

			std::pair<Data::BEDdataIt,Data::BEDdataIt> annoEntries;
			Data::BEDdataIt it;

			while (!fin.eof()) {

				unsigned maxB = max(100,(int)log2((double)mSignatureCounter)*100);
				unsigned currBuff = rand()%(maxB*3 - maxB + 1) + maxB;
				unsigned i = 0;
				bool lastSeqGr = false; // indicates that we have the last fragment from current seq, use to put all fragments from current seq into current chunk

				workQueueP myDataChunk = std::make_shared<workQueueS>();
				myDataChunk->gr.resize(currBuff);
				myDataChunk->names.resize(currBuff);
				myDataChunk->idx.resize(currBuff);
				//myDataChunk->offset = mInstanceCounter;
				myDataChunk->seqFile = myData;

				while ( ((i<currBuff) && !fin.eof()) || (myData->updateIndex==NONE && i>=currBuff && lastSeqGr == false) ) {

					//cout << "valid? " << valid_input << " name :" << currSeqName << ": pos " << pos << " end " << end <<  endl;
					if (!valid_input) {
						if  ( it == annoEntries.second ) {
							// last seq is finished, get next seq from file
							switch (myData->filetype) {
							case FASTA:
								mpData->GetNextFastaSeq(fin, currFullSeq, currSeqName);
								if (fin.eof() )
									continue;
								if (myData->updateIndex!= NONE && seq_names_seen.count(currSeqName) > 0) {
									throw range_error("Sequence names are not unique in FASTA file! "+currSeqName);
								} else if (myData->updateIndex!= NONE) {
									seq_names_seen.insert(make_pair(currSeqName,1));
								}
								break;
							case STRINGSEQ:
								mpData->GetNextStringSeq(fin, currFullSeq, currSeqName);
								if (fin.eof() )
									continue;
								break;
							default:
								throw range_error("ERROR Data::LoadData: file type not recognized: " + myData->filetype);
							}

							if (myData->updateIndex!= NONE){
								cout << endl << " next found Seq #" <<  seq_names_seen.size() << " length " << currFullSeq.size() << ":" << currSeqName << ": " << endl;
							}
							// if we have bed entries for that seq, find them and set iterator to first bed entry
							if (myData->dataBED && myData->dataBED->find(currSeqName) != myData->dataBED->end()){
								annoEntries = myData->dataBED->equal_range(currSeqName);
								it = annoEntries.first;
							} else if (myData->dataBED){
								cout << "no bed entry found! "<< seq_names_seen.size()<< endl;
								// bed is present, but no entry for current seq found -> we take next seq
								valid_input = false;
								continue;
							} else {
								// no bed is present, then we set start/end to full seq, eg. in case for clustering
								pos=0;
								end=currFullSeq.size();
								//cout << "no BED data present! "<< currSeqName << " " << pos << "-" << end << endl;
							}
						}

						// CLUSTER     F1:S1:W1 W2 W3  <-> 	 F1:S2:W1 W2 W3
						// CLASSIFY		F1:S1:W W W  ->  F1:S2:W W W

						// update index?
						// check under which value a current seq/window is inserted in inverse index
						// we only use these cases if there is a valid bed entry (SEQ_FEATURE/SEQ_NAME)
						multimap<string,uint>::iterator feat;
						switch (myData->updateIndex){
						// use seq name as value for inverse index
						case SEQ_NAME:
							feat = mFeature2IndexValue.find(currSeqName);
							if (feat != mFeature2IndexValue.end()){
								idx = feat->second;
							} else {
								myData->lastMetaIdx++;
								idx=myData->lastMetaIdx;
								mFeature2IndexValue.insert(make_pair(currSeqName,idx));
							}
							break;
							// use given value/name in BED file col4 as  value for inverse index
						case SEQ_FEATURE:
							feat = mFeature2IndexValue.find(it->second->NAME);
							if (feat != mFeature2IndexValue.end()){
								idx = feat->second;
							} else {
								myData->lastMetaIdx++;
								idx=myData->lastMetaIdx;
								mFeature2IndexValue.insert(make_pair(it->second->NAME,idx));
							}
							//cout << "update SEQ_FEATURE idx=" << idx << " " << it->second.NAME << endl;
							break;
						default:
							break;
						}

						// only true if we have a found a BED entry for current seq
						// set region according to BED entry
						if ( it != annoEntries.second ) {
							pos = it->second->START;
							end = it->second->END;
							//mIndexValue2Feature.insert(make_pair(idx,it->second));
							cout << endl << "BED entry found for seq name " << currSeqName << " " << it->second->NAME << " MetaIdx "<< idx << " " << pos << "-"<< end << endl;
							it++;
						}

						// check if start/end is within bounds of found seq
						if (pos>currSeq.size() || end > currFullSeq.size())
							throw range_error(" BED entry start/end is outside current seq ");

						// apply current seq start/end
						currSeq = currFullSeq.substr(pos,end-pos);

					} // valid_input?


					if (i>=currBuff){
						myDataChunk->gr.resize(i+1);
						myDataChunk->names.resize(i+1);
						myDataChunk->idx.resize(i+1);
					}
					// fill the current chunk
					mpData->SetGraphFromSeq2(myDataChunk->gr.at(i),currSeq, pos, lastSeqGr);
					if (myDataChunk->gr.at(i).IsEmpty()) {
						valid_input = false;
					} else {
						valid_input = true;
						mInstanceCounter++;
						file_instances++;

						myDataChunk->names.at(i) = currSeqName;
						if (myDataChunk->names.at(i) == "") {
							myDataChunk->names.at(i) = std::to_string(mInstanceCounter);
						}

						switch (myData->updateIndex){
						case NONE:
						case SEQ_WINDOW:
							idx = mInstanceCounter;
							break;
						default:
							break;
						}

						myDataChunk->idx.at(i) = idx;
						i++;

						// if (i-1==currBuff){ lastName=
					}
					//cout << "Gr: " << myDataChunk->gr.size() << " "<< i << " " << currBuff<< " "<< pos << " " << currSeqName<<  " " << currSeq.size() << " " << lastSeqGr << endl;
				} // while buffer not full or eof
				//cout << "Gr: " << myDataChunk->gr.size() << " "<< i << " " << currBuff<< " "<< pos << " " << currSeqName<<  " " << currSeq.size() << " " << lastSeqGr << endl;
				if (i==0)
					continue;

				myDataChunk->idx.resize(i);
				myDataChunk->gr.resize(i);
				myDataChunk->names.resize(i);

				graph_queue.push(myDataChunk);
				cv2.notify_all();
				if (mInstanceCounter%1000000 <=currBuff){
					cout << endl << " instances read " << mInstanceCounter << " " << myDataChunk->gr.size() << " buffer " << currBuff << " full..." << graph_queue.size() << " " << currSeq.size()<< " "<< currSeqName << endl;
				}
				if (graph_queue.size()>=numWorkers*25){
					unique_lock<mutex> lk(mut2);
					cv2.wait(lk,[&]{if ((done) || (graph_queue.size()<=numWorkers*10)) return true; else return false;});
					lk.unlock();
				}
				cv2.notify_all();
			}
			fin.close();
			files_done++;
			cout << files_done << " " << mInstanceCounter << " " << mSignatureCounter << " instances produced from file " << file_instances << endl;
		}
	}
}

void MinHashEncoder::worker_Graph2Signature(int numWorkers){

	while (!done){

		workQueueP myData;
		unique_lock<mutex> lk(mut2);
		cv2.wait(lk,[&]{if ( (done) ||  (graph_queue.try_pop( (myData) )) ) return true; else return false;});
		lk.unlock();

		if (!done && myData->gr.size()>0) {
			myData->sigs.resize(myData->gr.size());

			//cout << "  graph2sig thread got chunk " << myData->gr.size() << " offset " << myData->offset << " " << mpParameters->mHashBitSize << endl;

			for (unsigned j = 0; j < myData->gr.size(); j++) {
				SVector x(pow(2, mpParameters->mHashBitSize));
				generate_feature_vector(myData->gr[j], x);
				myData->sigs[j] = ComputeHashSignature(x);
			}
			if (sig_queue.size()>=numWorkers*25){
				unique_lock<mutex> lk(mut2);
				cv2.wait(lk,[&]{if ((done) || (sig_queue.size()<=numWorkers*10)) return true; else return false;});
				lk.unlock();
			}
			sig_queue.push(myData);
		}
		cv2.notify_all();
		cv3.notify_all();
	}
}

void MinHashEncoder::finisher(){
	ProgressBar progress_bar(1000);
	while (!done){

		workQueueP myData;
		unique_lock<mutex> lk(mut3);
		cv3.wait(lk,[&]{if ( (done) || (sig_queue.try_pop( (myData) ))) return true; else return false;});
		lk.unlock();

		if (!done && myData->sigs.size()>0) {

			uint chunkSize = myData->sigs.size();

			if (myData->seqFile->updateIndex != NONE){
				for (unsigned j = 0; j < myData->sigs.size(); j++) {
					UpdateInverseIndex(myData->sigs[j], myData->idx[j]);
				}
			} //else {
			// virtual function call that can be overloaded in child classes to do specific stuff
			finishUpdate(myData);
			//}

			mSignatureCounter += chunkSize;
			if (mSignatureCounter%1000000 <= chunkSize){
				cout << endl << "    finisher updated index with " << chunkSize << " signatures all_sigs=" <<  mSignatureCounter << " inst=" << mInstanceCounter << " sigQueue=" << sig_queue.size() << endl;
			}
			progress_bar.Count(mSignatureCounter);
		}
		cv1.notify_all();
		cv2.notify_all();
		cvm.notify_all();
	}
}

void MinHashEncoder::LoadData_Threaded(SeqFilesT& myFiles){

	for (unsigned i=0;i<myFiles.size(); i++){
		readFile_queue.push(myFiles[i]);
	}
	cout << "Using " << mpParameters->mHashBitSize << " bits to encode features" << endl;
	cout << "Using " << mpParameters->mRandomSeed << " as random seed" << endl;
	cout << "Using " << mpParameters->mNumHashFunctions << " hash functions (with factor " << mpParameters->mNumRepeatsHashFunction << " for single minhash)" << endl;
	cout << "Using " << mpParameters->mNumHashShingles << " as hash shingle factor" << endl;
	cout << "Using feature radius   " << mpParameters->mMinRadius<<".."<<mpParameters->mRadius << endl;
	cout << "Using feature distance " << mpParameters->mMinDistance<<".."<<mpParameters->mDistance << endl;
	cout << "Using sequence window  " << mpParameters->mSeqWindow<<" shift "<<mpParameters->mSeqShift << " ("<< (unsigned)std::max((double)1,(double)mpParameters->mSeqWindow*mpParameters->mSeqShift) << ") clip " << mpParameters->mSeqClip << endl;
	cout << "Computing MinHash signatures on the fly while reading " << myFiles.size() << " file(s)..." << endl;

	// threaded producer-consumer model for signature creation and index update
	// created threads:
	// 	1 finisher that updates the index and signature cache,
	// 	1 to read files and produces sequence instances
	//		n worker threads that create the signatures

	int graphWorkers = std::thread::hardware_concurrency();
	if (mpParameters->mNumThreads>0)
		graphWorkers = mpParameters->mNumThreads;

	cout << "Using " << graphWorkers << " worker threads and 2 helper threads..." << endl;

	done = false;
	files_done=0;
	mSignatureCounter = 0;
	mInstanceCounter = 0;

	vector<std::thread> threads;
	threads.push_back( std::thread(&MinHashEncoder::finisher,this));
	for (int i=0;i<graphWorkers;i++){
		threads.push_back( std::thread(&MinHashEncoder::worker_Graph2Signature,this,graphWorkers));
	}
	threads.push_back( std::thread(&MinHashEncoder::worker_readFiles,this,graphWorkers));

	{
		join_threads joiner(threads);

		unique_lock<mutex> lk(mutm);
		cv1.notify_all();
		while(!done){
			cvm.wait(lk,[&]{if ( (files_done<myFiles.size()) || (mSignatureCounter < mInstanceCounter)) return false; else return true;});
			lk.unlock();
			done=true;
			cv3.notify_all();
			cv2.notify_all();
			cv1.notify_all();
		}

	} // by leaving this block threads get joined by destruction of joiner

	// threads finished
	if (numKeys>0)
		cout << endl << "Inverse index ratio of overfull bins (maxSizeBin): " << ((double)numFullBins)/((double)numKeys) << " "<< numFullBins << "/" << numKeys << " instances " << mInstanceCounter << endl;

	if (mInstanceCounter == 0) {
		throw range_error("ERROR in MinHashEncoder::LoadData: something went wrong; no instances/signatures produced");
	} else
		cout << "Instances/signatures produced " << mInstanceCounter << endl;
}

unsigned MinHashEncoder::GetLoadedInstances() {
	return mInstanceCounter;
}


vector<unsigned> MinHashEncoder::ComputeHashSignature(SVector& aX) {

	unsigned numHashFunctionsFull = mpParameters->mNumHashFunctions * mpParameters->mNumHashShingles;
	unsigned sub_hash_range = numHashFunctionsFull / mpParameters->mNumRepeatsHashFunction;
	vector<unsigned> signature(numHashFunctionsFull);
	//init with MAXUNSIGNED
	for (unsigned k = 0; k < numHashFunctionsFull; ++k)
		signature[k] = MAXUNSIGNED;

	//prepare a vector containing the signature as the k min values
	//for each element of the sparse vector
	for (SVector::InnerIterator it(aX); it; ++it) {
		unsigned feature_id = it.index();
		//for each sub_hash
		for (unsigned l = 1; l <= mpParameters->mNumRepeatsHashFunction; ++l) {
			unsigned key = IntHash(feature_id, MAXUNSIGNED, l);
			for (unsigned kk = 0; kk < sub_hash_range; ++kk) { //for all k values
				unsigned lower_bound = MAXUNSIGNED / sub_hash_range * kk;
				unsigned upper_bound = MAXUNSIGNED / sub_hash_range * (kk + 1);
				// upper bound can be different from MAXUNSIGNED due to rounding effects, correct this
				//if (kk+1==sub_hash_range) upper_bound=MAXUNSIGNED;
				if (key >= lower_bound && key < upper_bound) { //if we are in the k-th slot
					unsigned signature_feature = kk + (l - 1) * sub_hash_range;
					if (key < signature[signature_feature]) //keep the min hash within the slot
						signature[signature_feature] = key;
				}
			}
		}
	}

	// compute shingles, i.e. rehash mNumHashShingles hash values into one hash value
	if (mpParameters->mNumHashShingles == 1 ) {
		return signature;
	} else {
		vector<unsigned> signatureFinal(mpParameters->mNumHashFunctions);
		for (unsigned i=0;i<mpParameters->mNumHashFunctions;i++){
			vector<unsigned> sigShinglet;
			for (unsigned j=i*mpParameters->mNumHashShingles;j<=i*mpParameters->mNumHashShingles+mpParameters->mNumHashShingles-1; j++){
				sigShinglet.push_back(signature[j]);
			}
			signatureFinal[i] = HashFunc(sigShinglet);
		}
		return signatureFinal;
	}
}

void NeighborhoodIndex::NeighborhoodCacheReset() {
	cout << "... nearest neighbor cache reset ..." << endl;
	mNeighborhoodCache.clear();
	mNeighborhoodCacheExt.clear();
	mNeighborhoodCacheInfo.clear();

	mNeighborhoodCache.resize(GetLoadedInstances());
	mNeighborhoodCacheExt.resize(GetLoadedInstances());
	mNeighborhoodCacheInfo.resize(GetLoadedInstances());
}


vector<unsigned>& NeighborhoodIndex::ComputeHashSignature(unsigned aID) {
	cout << "here2" << aID << endl;
	if (mMinHashCache && mMinHashCache->size()>0 && mMinHashCache->at(aID).size() > 0)
		return mMinHashCache->at(aID);
	else {
		throw range_error("ERROR: MinHashCache is not filled!");
	}
}


void NeighborhoodIndex::UpdateInverseIndex(vector<unsigned>& aSignature, unsigned aIndex) {
	for (unsigned k = 0; k < mpParameters->mNumHashFunctions; ++k) { //for every hash value
		unsigned key = aSignature[k];
		if (key != MAXUNSIGNED && key != 0) { //if key is equal to markers for empty bins then skip insertion instance in data structure
			if (mInverseIndex[k].count(key) == 0) { //if this is the first time that an instance exhibits that specific value for that hash function, then store for the first time the reference to that instance
				indexBinTy tmp;
				tmp.push_back(aIndex);
				mInverseIndex[k].insert(make_pair(key, tmp));
				numKeys++; // just for bin statistics
			} else if (mInverseIndex[k][key].size() < mpParameters->mMaxSizeBin && mInverseIndex[k][key].front() != MAXBINKEY) {
				// add key to bin if we not have a full bin
				mInverseIndex[k][key].push_back(aIndex);
			} else if (mInverseIndex[k][key].size() == mpParameters->mMaxSizeBin){
				// if a bin is full we clear it and add a key with MAXUNSIGNED to indicate an overfull bin
				numFullBins++; // just for bin statistics
				mInverseIndex[k][key].clear();
				mInverseIndex[k][key].push_back(MAXBINKEY);
			}
		}
	}
}


void NeighborhoodIndex::ComputeApproximateNeighborhoodCore(const vector<unsigned>& aSignature, umap_uint_int& neighborhood, unsigned& collisions) {

	collisions = 0;
	for (unsigned k = 0; k < mpParameters->mNumHashFunctions; ++k) {

		unsigned hash_id = aSignature[k];
		if (hash_id != 0 && hash_id != MAXUNSIGNED && mInverseIndex[k][hash_id].front() != MAXBINKEY) {

			//fill neighborhood set counting number of occurrences
			for (indexBinTy::iterator it = mInverseIndex[k][hash_id].begin(); it != mInverseIndex[k][hash_id].end(); ++it) {
				binKeyTy instance_id = *it;
				if (neighborhood.count(instance_id) > 0)
					neighborhood[instance_id]++;
				else
					neighborhood[instance_id] = 1;
			}
		} else {
			collisions++;
		}
	}
}

vector<unsigned> NeighborhoodIndex::ComputeApproximateNeighborhood(const vector<unsigned>& aSignature, unsigned& collisions, double& density) {

	umap_uint_int neighborhood;
	collisions = 0;
	density = 0;
	ComputeApproximateNeighborhoodCore(aSignature,neighborhood,collisions);
	//	cout <<"here "<< aSignature.size() << " " << collisions << " " << mpParameters->mMaxSizeBin << endl;
	return TrimNeighborhood(neighborhood, collisions, density);
}


umap_uint_int NeighborhoodIndex::ComputeApproximateNeighborhoodExt(const vector<unsigned>& aSignature, unsigned& collisions, double& density) {

	umap_uint_int neighborhood;
	collisions = 0;

	ComputeApproximateNeighborhoodCore(aSignature,neighborhood,collisions);
	vector<unsigned> myN = TrimNeighborhood(neighborhood, collisions, density);

	return neighborhood;
}

vector<unsigned> NeighborhoodIndex::TrimNeighborhood(umap_uint_int& aNeighborhood, unsigned collisions, double& density) {

	vector<unsigned> neighborhood_list;
	density = 0;
	double myC = (mpParameters->mPureApproximateSim * (mpParameters->mNumHashFunctions - collisions));
	//cout << "here 2 myC "<< myC << " nsize " << aNeighborhood.size() << " coll " << collisions << endl;
	for (umap_uint_int::const_iterator it = aNeighborhood.begin(); it != aNeighborhood.end();) {
		if ( (double)it->second >= myC ) {
			neighborhood_list.push_back(it->first);
			density += it->second;
			it++;
		} else {
			it=aNeighborhood.erase(it);
		}
	}
	if (collisions < mpParameters->mNumHashFunctions && neighborhood_list.size()!=0){
		density = ( density / neighborhood_list.size() ) / (mpParameters->mNumHashFunctions - collisions);
	} else
		density = 0;

	return neighborhood_list;
}

vector<unsigned> NeighborhoodIndex::ComputeNeighborhood(const unsigned aID, unsigned& collisions, double& density) {

	vector<unsigned> neighborhood_list;

	if (mNeighborhoodCache[aID].size() != 0) {
		neighborhood_list = mNeighborhoodCache[aID];
		pair<unsigned, double> tmp = mNeighborhoodCacheInfo[aID];
		collisions = tmp.first;
		density = tmp.second;
	} else {
		vector<unsigned> signature = ComputeHashSignature(aID);
		vector<unsigned> neighborhood_list = ComputeApproximateNeighborhood(signature, collisions, density);
		mNeighborhoodCacheInfo[aID] = make_pair(collisions,density);
		mNeighborhoodCache[aID] = neighborhood_list;
	}
	return neighborhood_list;
}

umap_uint_int NeighborhoodIndex::ComputeNeighborhoodExt(unsigned aID, unsigned& collisions, double& density) {
	//cache neighborhoods (if opted for)
	umap_uint_int neighborhood_list;
	if (mNeighborhoodCacheExt[aID].size() != 0) {
		neighborhood_list = mNeighborhoodCacheExt[aID];
		pair<unsigned, double> tmp = mNeighborhoodCacheInfo[aID];
		collisions = tmp.first;
		density = tmp.second;
	} else {
		vector<unsigned> signature = ComputeHashSignature(aID);
		umap_uint_int neighborhood_list = ComputeApproximateNeighborhoodExt(signature, collisions, density);
		mNeighborhoodCacheExt[aID] = neighborhood_list;
		mNeighborhoodCacheInfo[aID] = make_pair(collisions,density);
	}
	return neighborhood_list;
}


double NeighborhoodIndex::ComputeApproximateSim(const unsigned& aID, const unsigned& bID) {

	vector<unsigned> signatureA = ComputeHashSignature(aID);
	vector<unsigned> signatureB = ComputeHashSignature(bID);

	pair<unsigned,unsigned> simAB = ComputeApproximateSim(aID,signatureB);
	pair<unsigned,unsigned> simBA = ComputeApproximateSim(bID,signatureA);

	return (double)(simAB.first + simBA.first)/(2*mpParameters->mNumHashFunctions-simAB.second-simBA.second);
}


pair<unsigned,unsigned> NeighborhoodIndex::ComputeApproximateSim(const unsigned& aID, const vector<unsigned>& bSignature) {

	//umap_uint_int neighborhood;
	unsigned collisions = 0;
	unsigned counts_aID = 0;
	for (unsigned k = 0; k < mpParameters->mNumHashFunctions; ++k) {
		unsigned hash_id = bSignature[k];
		if (hash_id != 0 && hash_id != MAXUNSIGNED && mInverseIndex[k][hash_id].front() != MAXBINKEY) {

			//fill neighborhood set counting number of occurrences
			for (indexBinTy::iterator it = mInverseIndex[k][hash_id].begin(); it != mInverseIndex[k][hash_id].end(); ++it) {
				if (*it == aID) counts_aID++;
			}

		} else
			collisions++;
	}
	pair<unsigned,unsigned> tmp=make_pair(counts_aID,collisions);
	return tmp;
}

void HistogramIndex::SetHistogramSize(binKeyTy size){
	mHistogramSize = size;
}


HistogramIndex::binKeyTy HistogramIndex::GetHistogramSize(){
	return mHistogramSize;
}


//bool compare(const Data::SeqFileT& first, const Data::SeqFileT& second) {
//
//	return (first.uIdx<second.uIdx);
//}

//void  HistogramIndex::PrepareIndexDataSets(vector<SeqDataSet>& myFileList){
//
//	if (!myFileList.size())
//		throw range_error("ERROR no datasets to prepare ...");
//
//	sort(myFileList.begin(),myFileList.end(),compare);
//
//	// check if we have datasets to update the index
//	uint bin = 0;
//	uint userIdx = myFileList[0].uIdx;
//	for (unsigned i=0;i<myFileList.size(); i++){
//		if (myFileList[i].updateIndex){
//			if ( myFileList[i].uIdx != userIdx )
//				bin++;
//			mHistBin2DatasetIdx.insert(make_pair(bin,i));
//			userIdx = myFileList[i].uIdx;
//			myFileList[i].idx = bin;
//			mIndexDataSets.push_back(myFileList[i]);
//			//cout << "bin" << bin << " " << userIdx << " "<<myFileList[i].idx << " " << myFileList[i].uIdx << endl;
//		}
//	}
//	SetHistogramSize(bin+1);
//}

void HistogramIndex::UpdateInverseIndex(vector<unsigned>& aSignature, unsigned aIndex) {
	for (unsigned k = 0; k < mpParameters->mNumHashFunctions; ++k) { //for every hash value
		unsigned key = aSignature[k];
		if (key != MAXUNSIGNED && key != 0) { //if key is equal to markers for empty bins then skip insertion instance in data structure
			if (mInverseIndex[k].count(key) == 0) { //if this is the first time that an instance exhibits that specific value for that hash function, then store for the first time the reference to that instance

				binKeyTy * foo;
				foo = new binKeyTy[2];
				foo[1]= (binKeyTy)aIndex;
				foo[0]= 1; //index of last element is stored at idx[0]

				mInverseIndex[k][key] = foo;
				numKeys++; // just for bin statistics
			} else if (mInverseIndex[k][key][mInverseIndex[k][key][0]] != (binKeyTy)aIndex){

				// find pos for insert, assume sorted array
				binKeyTy i = mInverseIndex[k][key][0];
				while ((mInverseIndex[k][key][i]> aIndex) && (i>1)){
					i--;
				}

				// only insert if element is not there
				if (mInverseIndex[k][key][i]<aIndex){
					binKeyTy newSize = (mInverseIndex[k][key][0])+1;
					binKeyTy * fooNew;
					fooNew = new binKeyTy[newSize+1];

					memcpy(fooNew,mInverseIndex[k][key],(i+1)*sizeof(binKeyTy));
					fooNew[i+1] = (binKeyTy)aIndex;
					memcpy(&fooNew[i+2],&mInverseIndex[k][key][i+1],(mInverseIndex[k][key][0]-i)*sizeof(binKeyTy));
					fooNew[0] = newSize;

					delete[] mInverseIndex[k][key];
					mInverseIndex[k][key] = fooNew;
				}
				//				cout << "bin " << key << " k " <<  k << " aIdx "<< aIndex << "\t";
				//				for (unsigned j=0; j<=mInverseIndex[k][key][0];j++){
				//					cout << mInverseIndex[k][key][j] <<"\t";
				//				}
				//				cout << endl;

			}
		}
	}
}


void HistogramIndex::ComputeHistogram(const vector<unsigned>& aSignature, std::valarray<double>& hist, unsigned& emptyBins) {

	hist.resize(GetHistogramSize());
	hist *= 0;
	emptyBins = 0;
	for (unsigned k = 0; k < aSignature.size(); ++k) {
		if (mInverseIndex[k].count(aSignature[k]) > 0) {

			std::valarray<double> t(0.0, hist.size());

			for (uint i=1;i<=mInverseIndex[k][aSignature[k]][0];i++){
				t[mInverseIndex[k][aSignature[k]][i]-1]=1;
			}

			hist += t;

		} else {
			emptyBins++;
		}
	}
}

void HistogramIndex::writeBinaryIndex2(ostream &out, const indexTy& index) {
	// create binary reverse index representation
	// format:
	out.write((const char*) &mpParameters->mHashBitSize, sizeof(unsigned));
	out.write((const char*) &mpParameters->mRandomSeed, sizeof(unsigned));
	out.write((const char*) &mpParameters->mRadius, sizeof(unsigned));
	out.write((const char*) &mpParameters->mMinRadius, sizeof(unsigned));
	out.write((const char*) &mpParameters->mDistance, sizeof(unsigned));
	out.write((const char*) &mpParameters->mMinDistance, sizeof(unsigned));
	out.write((const char*) &mpParameters->mNumHashShingles, sizeof(unsigned));
	out.write((const char*) &mpParameters->mNumRepeatsHashFunction, sizeof(unsigned));
	out.write((const char*) &mpParameters->mSeqWindow, sizeof(unsigned));
	out.write(reinterpret_cast<char *>(&mpParameters->mIndexSeqShift), sizeof(mpParameters->mIndexSeqShift));
	unsigned tmp = GetHistogramSize();
	out.write((const char*) &tmp, sizeof(unsigned));

	if (mFeature2IndexValue.size() != GetHistogramSize()){
		throw range_error("Ups! Histogramsize is different to mFeature2IndexValue.size()");
	}

	std::pair<string,uint> highest = *mFeature2IndexValue.rbegin();          // last element
	std::map<string,uint>::iterator it = mFeature2IndexValue.begin();
	do {
		out.write((const char*) &(it->second), sizeof(unsigned));
		tmp = it->first.size();
		out.write((const char*) &(tmp), sizeof(unsigned));
		out.write(const_cast<char*>(it->first.c_str()), it->first.size());
	} while ( mFeature2IndexValue.value_comp()(*it++, highest) );

	unsigned numHashFunc = index.size();
	out.write((const char*) &numHashFunc, sizeof(unsigned));
	for (typename indexTy::const_iterator it = index.begin(); it!= index.end(); it++){
		unsigned numBins = it->size();
		out.write((const char*) &numBins, sizeof(unsigned));
		for (typename indexSingleTy::const_iterator itBin = it->begin(); itBin!=it->end(); itBin++){
			unsigned binId = itBin->first;

			unsigned numBinEntries = itBin->second[0];
			out.write((const char*) &binId, sizeof(unsigned));
			out.write((const char*) &numBinEntries, sizeof(unsigned));

			for (binKeyTy i=1;i<=numBinEntries;i++){
				binKeyTy s = itBin->second[i];
				out.write((const char*) &(s), sizeof(binKeyTy));
			}
		}
	}
}

bool HistogramIndex::readBinaryIndex2(string filename, indexTy &index){
	igzstream fin;
	fin.open(filename.c_str());
	unsigned tmp;
	fin.read((char*) &mpParameters->mHashBitSize, sizeof(unsigned));
	fin.read((char*) &mpParameters->mRandomSeed, sizeof(unsigned));
	fin.read((char*) &mpParameters->mRadius, sizeof(unsigned));
	fin.read((char*) &mpParameters->mMinRadius, sizeof(unsigned));
	fin.read((char*) &mpParameters->mDistance, sizeof(unsigned));
	fin.read((char*) &mpParameters->mMinDistance, sizeof(unsigned));
	fin.read((char*) &mpParameters->mNumHashShingles, sizeof(unsigned));
	fin.read((char*) &mpParameters->mNumRepeatsHashFunction, sizeof(unsigned));
	fin.read((char*) &mpParameters->mSeqWindow, sizeof(unsigned));
	fin.read( reinterpret_cast<char*>( &mpParameters->mIndexSeqShift ), sizeof mpParameters->mIndexSeqShift);
	fin.read((char*) &tmp, sizeof(unsigned));
	SetHistogramSize(tmp);

	mFeature2IndexValue.clear();
	for (unsigned idx=1;idx<=GetHistogramSize();idx++){
		unsigned hist_idx;
		unsigned size;

		fin.read((char*) &hist_idx, sizeof(unsigned));
		fin.read((char*) &size, sizeof(unsigned));
		string feature;
		feature.resize(size);
		fin.read(const_cast<char*>(feature.c_str()), size);
		mFeature2IndexValue.insert(make_pair(feature,hist_idx));
	}

	unsigned numHashFunc = 0;
	fin.read((char*) &numHashFunc, sizeof(unsigned));
	if (numHashFunc <= 0)
		fin.setstate(std::ios::badbit);
	if (!fin.good())
		return false;
	index.resize(numHashFunc);
	for (unsigned  hashFunc = 0; hashFunc < numHashFunc; hashFunc++){

		unsigned numBins = 0;
		fin.read((char*) &numBins, sizeof(unsigned));
		if (numBins < 0)
			fin.setstate(std::ios::badbit);
		if (!fin.good())
			return false;
		index[hashFunc].resize(numBins);
		for (unsigned  bin = 0; bin < numBins; bin++){

			unsigned binId = 0;
			unsigned numBinEntries = 0;
			fin.read((char*) &binId, sizeof(unsigned));
			fin.read((char*) &numBinEntries, sizeof(unsigned));
			if (numBinEntries < 0 || binId <= 0)
				fin.setstate(std::ios::badbit);
			if (!fin.good())
				return false;

			indexBinTy tmp = new binKeyTy[numBinEntries+1];

			for (unsigned entry = 1; entry <= numBinEntries; entry++ ){

				binKeyTy s;
				fin.read((char*) &s, sizeof(binKeyTy));
				if (s < 0)
					fin.setstate(std::ios::badbit);
				if (!fin.good())
					return false;
				tmp[entry] = s;
			}
			tmp[0] = numBinEntries;
			index[hashFunc][binId] = tmp;
		}
	}
	fin.close();
	return true;
}

//void HistogramIndex::UpdateInverseIndex(vector<unsigned>& aSignature, unsigned aIndex) {
//	for (unsigned k = 0; k < mpParameters->mNumHashFunctions; ++k) { //for every hash value
//		unsigned key = aSignature[k];
//		if (key != MAXUNSIGNED && key != 0) { //if key is equal to markers for empty bins then skip insertion instance in data structure
//			if (mInverseIndex[k].count(key) == 0) { //if this is the first time that an instance exhibits that specific value for that hash function, then store for the first time the reference to that instance
//			/*	indexBinTy t(1,aIndex);
//				t.reserve(1);
//				mInverseIndex[k][key]= t;*/
//				std::unique_ptr<bin> myP(new bin);
//				myP->val=aIndex;
//				mInverseIndex[k][key] = std::move(myP);
//				//mInverseIndex[k][key].reserve(1);
//				numKeys++; // just for bin statistics
//			} else if (mInverseIndex[k][key].back() != aIndex){
//				mInverseIndex[k][key].push_back(aIndex);
//			}
//		}
//	}
//}


//void HistogramIndex::ComputeHistogram(const vector<unsigned>& aSignature, std::valarray<double>& hist, unsigned& emptyBins) {
//
//	hist.resize(GetHistogramSize());
//	hist *= 0;
//	emptyBins = 0;
//
//	for (unsigned k = 0; k < aSignature.size(); ++k) {
//		if (mInverseIndex[k].count(aSignature[k]) > 0) {
//
//			std::valarray<double> t(0.0, hist.size());
//
//			for (typename indexBinTy::const_iterator it = mInverseIndex[k][aSignature[k]].begin(); it != mInverseIndex[k][aSignature[k]].end();it++){
//				t[*it]=1;
//			}
//
//			hist += t;
//
//		} else {
//			emptyBins++;
//		}
//	}
//}

//double indicator(double i){if (i>0) return 1.0; else return 0.0;}

//void  HistogramIndex::ComputeHistogram(const vector<unsigned>& aSignature, std::valarray<double>& hist, unsigned& emptyBins) {
//
//	hist.resize(mIndexDataSets.size());
//	hist *= 0;
//	emptyBins = 0;
//
//	for (unsigned k = 0; k < aSignature.size(); ++k) {
//		if (mInverseIndex[k].count(aSignature[k]) > 0) {
//			vector<double> md(mInverseIndex[k][aSignature[k]].begin(),mInverseIndex[k][aSignature[k]].end());
//			std::valarray<double> t(md.data(),hist.size() );
//			std::valarray<double> ti = t.apply(indicator);
//			hist += ti;
//		} else {
//			emptyBins++;
//		}
//	}
//}


//bool HistogramIndex::readBinaryIndex(string filename, indexTy &index){
//	igzstream fin;
//	fin.open(filename.c_str());
//	unsigned numHashFunc = 0;
//	fin.read((char*) &numHashFunc, sizeof(unsigned));
//	if (numHashFunc <= 0)
//		fin.setstate(std::ios::badbit);
//	if (!fin.good())
//		return false;
//	index.resize(numHashFunc);
//	for (unsigned  hashFunc = 0; hashFunc < numHashFunc; hashFunc++){
//
//		unsigned numBins = 0;
//		fin.read((char*) &numBins, sizeof(unsigned));
//		if (numBins < 0)
//			fin.setstate(std::ios::badbit);
//		if (!fin.good())
//			return false;
//		for (unsigned  bin = 0; bin < numBins; bin++){
//
//			unsigned binId = 0;
//			unsigned numBinEntries = 0;
//			fin.read((char*) &binId, sizeof(unsigned));
//			fin.read((char*) &numBinEntries, sizeof(unsigned));
//			if (numBinEntries < 0 || binId <= 0)
//				fin.setstate(std::ios::badbit);
//			if (!fin.good())
//				return false;
//			indexBinTy tmp(numBinEntries);
//			index[hashFunc].insert(make_pair(binId,tmp));
//
//			for (unsigned entry = 0; entry < numBinEntries; entry++ ){
//
//				binTy t = 0;
//				fin.read((char*) &t, sizeof(binTy));
//				if (t < 0)
//					fin.setstate(std::ios::badbit);
//				if (!fin.good())
//					return false;
//				index[hashFunc][binId][entry]=t;
//			}
//		}
//	}
//	fin.close();
//	return true;
//}



//void HistogramIndex::UpdateInverseIndexHist(vector<unsigned>& aSignature, unsigned aIndex) {
//	for (unsigned k = 0; k < mpParameters->mNumHashFunctions; ++k) { //for every hash value
//		unsigned key = aSignature[k];
//		if (key != MAXUNSIGNED && key != 0) { //if key is equal to markers for empty bins then skip insertion instance in data structure
//			if (mInverseIndex[k].count(key) == 0) { //if this is the first time that an instance exhibits that specific value for that hash function, then store for the first time the reference to that instance
//				indexBinTy tmp(mIndexDataSets.size(),0);
//				tmp[aIndex]++;
//				mInverseIndex[k].insert(make_pair(key, tmp));
//				numKeys++; // just for bin statistics
//			} else if (mInverseIndex[k][key][aIndex]<MAXBIN){
//				mInverseIndex[k][key][aIndex]++;
//			}
//		}
//	}
//}


//void MinHashEncoder::CleanUpInverseIndex() {
//	for (unsigned k = 0; k < mpParameters->mNumHashFunctions; ++k) {
//		for (indexSingleTy::const_iterator jt = mInverseIndex[k].begin(); jt != mInverseIndex[k].end(); ++jt) {
//			unsigned hash_id = jt->first;
//			if (hash_id != 0 && hash_id != MAXUNSIGNED) { //do not consider buckets corresponding to null bins
//				unsigned collision_size = mInverseIndex[k][hash_id].size();
//
//				if (collision_size < mpParameters->mMaxSizeBin) {
//				} else {//remove bins that are too full from inverse index
//					mInverseIndex[k].erase(hash_id);
//				}
//			}
//		}
//	}
//}

//vector<unsigned> MinHashEncoder::ComputeHashSignatureSize(vector<unsigned>& aSignature) {
//	vector<unsigned> signature_size(mpParameters->mNumHashFunctions);
//	assert(aSignature.size()==mpParameters->mNumHashFunctions);
//	for (unsigned i = 0; i < aSignature.size(); ++i) {
//		unsigned key = aSignature[i];
//		signature_size[i] = mInverseIndex[i][key].size();
//	}
//	return signature_size;
//}

//void HistogramIndex::writeBinaryIndex(ostream &out, const indexTy& index) {
//	// create binary reverse index representation
//	// format:
//	unsigned numHashFunc = index.size();
//	out.write((const char*) &numHashFunc, sizeof(unsigned));
//	for (indexTy::const_iterator it = index.begin(); it!= index.end(); it++){
//		unsigned numBins = it->size();
//		out.write((const char*) &numBins, sizeof(unsigned));
//		for (indexSingleTy::const_iterator itBin = it->begin(); itBin!=it->end(); itBin++){
//			unsigned binId = itBin->first;
//			unsigned numBinEntries = itBin->second.size();
//			out.write((const char*) &binId, sizeof(unsigned));
//			out.write((const char*) &numBinEntries, sizeof(unsigned));
//			for (indexBinTy::const_iterator binEntry = itBin->second.begin(); binEntry != itBin->second.end(); binEntry++){
//				binTy t= *binEntry;
//				out.write((const char*) &(t), sizeof(binTy));
//			}
//		}
//	}
//}
