#include "ClusterManager.h"
#include <ostream>

ClusterManager::ClusterManager(Parameters* apParameters, Data* apData) :
BaseManager(apParameters, apData), mNearestNeighbor(apParameters, apData) {
}


//---------------------------------------------------------------------------------------------------
// APPROX_DENSE_CENTERS
ApproxDensityClusterManager::ApproxDensityClusterManager(Parameters* apParameters, Data* apData): ClusterManager(apParameters, apData)
				 {
	SeqDataSet mySet;
	vector<SeqDataSet> myList;
	mySet.filename = mpParameters->mInputDataFileName.c_str();
	mySet.idx = 1;
	mySet.filetype = mpParameters->mFileTypeCode;
	mySet.desc = "approx_cluster_set";
	myList.push_back(mySet);
	mNearestNeighbor.mMinHashEncoder.LoadDataIntoIndexThreaded(myList,true,NULL);
	//mNearestNeighbor.mMinHashEncoder.LoadDataIntoIndex();

	mNearestNeighbor.CacheReset();
}

void ApproxDensityClusterManager::Exec() {
	ProgressBar pb;
	OutputManager on("approx_dense_centers", mpParameters->mDirectoryPath);
	OutputManager oc("approx_dense_cluster", mpParameters->mDirectoryPath);

	DenseCluster(oc.mOut,on.mOut);

	cout << endl << "Approx dense cluster results written in file " << oc.GetFullPathFileName() << endl;
	cout << "Approx dense center results written in file " << on.GetFullPathFileName() << endl;
	pb.Count();
	cout << endl << "Total clustering time:" << endl;
}

void ApproxDensityClusterManager::DenseCluster(ostream& out_c, ostream& out_n) {
	cout << endl << "Compute neighborhood and density for selected " << mpData->mRowIndexList.size() << " instances." << endl;
	vector<pair<double, unsigned> > DensityList(mpData->mRowIndexList.size());

	{
		ProgressBar ppb(1000);
#ifdef USEMULTITHREAD
#pragma omp parallel for schedule(dynamic,100)
#endif
		for (unsigned i = 0; i < mpData->mRowIndexList.size(); ++i) {
			unsigned ii = mpData->mRowIndexList[i];

			//compute neighbors
			vector<unsigned> neighborhood_list;
			unsigned collisions = 0;
			double density = 0;

			neighborhood_list = mNearestNeighbor.ComputeNeighborhood(ii,collisions,density);

			double score = density * neighborhood_list.size();
			DensityList[ii]= make_pair(-score,ii);
			ppb.Count();
		}
	}
	cout << endl << " *** Approx-Density Clustering *** " << endl;
	cout << "sort instances according to neighborhood density score..." << endl;

	sort(DensityList.begin(), DensityList.end());

	vector<unsigned> neighborhood_list;
	unsigned collisions = 0;
	double density = 0;
	vector<long int> adjacencyList(mpData->mRowIndexList.size());

	for (unsigned k = 0; k < adjacencyList.size(); ++k) {
		adjacencyList[k] = -1;
	}

	cout << "make adjacency list..." << endl;
	for (	unsigned i = 0; i < DensityList.size(); ++i) {
		unsigned ii = mpData->mRowIndexList[DensityList[i].second];
		// cluster seed seq is not yet (-1) part of any cluster
		if ( adjacencyList[ii] == -1 ) {
			neighborhood_list = mNearestNeighbor.ComputeNeighborhood(ii,collisions,density);

			// count not clustered instances of neighborhood(ii)
			// and also the cluster-frequency of already clustered instances
			umap_uint_int clustered;
			unsigned notClustered = 0;
			for (vector<unsigned>::const_iterator it = neighborhood_list.begin(); it != neighborhood_list.end(); ++it) {
				if ( adjacencyList[*it] != -1 ) {
					if (clustered.count(adjacencyList[*it]) == 0 ){
						clustered.insert(make_pair(adjacencyList[*it],1));
					} else {
						clustered[adjacencyList[*it]]++;
					}
				} else {
					notClustered++;
				}
			}

			unsigned newClusId = ii;
			if (notClustered<neighborhood_list.size()){
				vector<pair<signed,unsigned> > sortedAb;
				for (umap_uint_int::iterator it = clustered.begin(); it != clustered.end(); it++){
					sortedAb.push_back(make_pair(-it->second,it->first));
				}

				sort(sortedAb.begin(),sortedAb.end());

				// merge not clustered instances of nh(ii) into most abundant cluster (sortedAb[0].second) of nh(ii) if:
				// a) all already clustered instances belong to the same cluster
				// b) neighborhood size >=6  and less than half instances are not yet clustered
				if (-sortedAb.front().first >= (signed)(neighborhood_list.size() - notClustered) && (notClustered+2)*2 <= neighborhood_list.size()){
					newClusId = sortedAb[0].second;
				}
			}

			// assign cluster id to all not clustered instances
			for (vector<unsigned>::const_iterator it = neighborhood_list.begin(); it != neighborhood_list.end(); ++it) {
				if ( adjacencyList[*it] == -1 ) {
					adjacencyList[*it] = newClusId;
				}
			}
			adjacencyList[ii] = newClusId;
		}
	}

	// build clusters
	umap_uint_vec_uint clusters;
	for (	unsigned i = 0; i < adjacencyList.size(); ++i) {
		if ( adjacencyList[i] != -1 ) {
			clusters[adjacencyList[i]].push_back(i);
		} else {
			clusters[i].push_back(i);
		}
	}

	cout << "Found " << clusters.size() << " clusters - output them now ..." << endl;

	for (umap_uint_vec_uint::const_iterator it = clusters.begin(); it != clusters.end(); ++it) {
		out_c << it->first << "    ";
		for (vector<unsigned>::const_iterator mIt = it->second.begin(); mIt != it->second.end(); ++mIt) {
			out_c << *mIt << " ";
		}
		out_c << endl;
	}

	//output all info: id of neighbour, similarity of neighbor, target of neighbor
	cout << "Output all ranked Neighborhoods..." << endl;
	for (	unsigned i = 0; i < DensityList.size(); ++i) {
		unsigned ii = mpData->mRowIndexList[DensityList[i].second];
		out_n << ii << ":" << -DensityList[i].first <<  "    ";
		neighborhood_list = mNearestNeighbor.ComputeNeighborhood(ii,collisions,density);
		for (vector<unsigned>::const_iterator it = neighborhood_list.begin(); it != neighborhood_list.end(); ++it) {
			out_n << *it << " ";
		}
		out_n << endl;
	}
}

//---------------------------------------------------------------------------------------------------