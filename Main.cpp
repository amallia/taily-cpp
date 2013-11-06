#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <algorithm>
#include <string>
#include <db_cxx.h>
#include <boost/math/distributions/gamma.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>

#include "indri/QueryEnvironment.hpp"
#include "indri/Repository.hpp"
#include "indri/CompressedCollection.hpp"
#include "indri/ScopedLock.hpp"

#include "FeatureStore.h"
#include "ShardRanker.h"

char* getOption(char ** begin, char ** end, const std::string & option) {
  char ** itr = std::find(begin, end, option);
  if (itr != end && ++itr != end) {
    return *itr;
  }
  return 0;
}

bool hasOption(char** begin, char** end, const std::string& option) {
  return std::find(begin, end, option) != end;
}

void readParams(const char* paramFile, map<string, string> *params) {
  ifstream file;
  file.open(paramFile);

  string line;
  if (file.is_open()) {

    while (getline(file, line)) {
      char mutableLine[line.size() + 1];
      std::strcpy(mutableLine, line.c_str());

      char* key = std::strtok(mutableLine, "=");
      char* value = std::strtok(NULL, "=");
      (*params)[key] = value;

      std::cout << line << endl;
    }
    file.close();
  }
}


double calcIndriFeature(double tf, double ctf, double totalTermCount, double docLength, int mu = 2500) {
  return log( (tf + mu*(ctf/totalTermCount)) / (docLength + mu) );
}

void tokenize(string line, const char * delim, vector<string>* output) {
  char mutableLine[line.size() + 1];
  std::strcpy(mutableLine, line.c_str());
  for (char* value = std::strtok(mutableLine, delim); 
      value != NULL; 
      value = std::strtok(NULL, delim)) {
    output->push_back(value);
  }
}

int main(int argc, char * argv[]) {
  int MU = 2500;

  char* paramFile = getOption(argv, argv + argc, "-p");

  // read parameter file
  std::map<string, string> params;
  readParams(paramFile, &params);

  if (strcmp(argv[1], "buildshard") == 0) {

    string dbPath = params["db"];
    string indexPath = params["index"];
    string corpusDbPath = params["corpusDb"];

    // open corpus statistics db
    FeatureStore corpusStats(corpusDbPath, true);

    // create and open the data store
    FeatureStore store(dbPath, false);

    indri::collection::Repository repo;
    repo.openRead(indexPath);

    indri::collection::Repository::index_state state = repo.indexes();

    if (state->size() > 1) {
      cout << "Index has more than 1 part. Can't deal with this, man.";
      exit(EXIT_FAILURE);
    }

    for(size_t i = 0; i < state->size(); i++) {
      using namespace indri::index;
      Index* index = (*state)[i];
      indri::thread::ScopedLock( index->iteratorLock() );

      DocListFileIterator* iter = index->docListFileIterator();
      iter->startIteration();
      cout << index->termCount() << " " << index->documentCount() << endl;

      // get the total term length of the collection (for Indri scoring)
      double totalTermCount = index->termCount();
      string totalTermCountKey(FeatureStore::TERM_SIZE_FEAT_SUFFIX);
      corpusStats.getFeature((char*)totalTermCountKey.c_str(), &totalTermCount);

      // store the shard size (# of docs) feature
      int shardSizeFeat = index->documentCount();
      string featSize(FeatureStore::SIZE_FEAT_SUFFIX);
      store.putFeature((char*)featSize.c_str(), (double)shardSizeFeat, shardSizeFeat);

      // for each stem in the index
      while (!iter->finished()) {
        DocListFileIterator::DocListData* entry = iter->currentEntry();
        TermData* termData = entry->termData;

        // get ctf of term from corpus-wide stats Db
        double ctf;
        string ctfKey(termData->term);
        ctfKey.append(FeatureStore::TERM_SIZE_FEAT_SUFFIX);
        corpusStats.getFeature((char*)ctfKey.c_str(), &ctf);

        double featSum = 0.0f;
        double squaredFeatSum = 0.0f;
        double minFeat = DBL_MAX;

        entry->iterator->startIteration();

        // calculate Sum(f) and Sum(f^2) top parts of eq (3) (4)
        while (!entry->iterator->finished()) {
          DocListIterator::DocumentData* doc = entry->iterator->currentEntry();
          double length = index->documentLength(doc->document);
          double tf = doc->positions.size();

          // calulate Indri score feature and sum it up
          double feat = calcIndriFeature(tf, ctf, totalTermCount, length);
          featSum += feat;
          squaredFeatSum += pow(feat, 2);

          // keep track of this shard's minimum feature
          if (feat < minFeat) {
            minFeat = feat;
          }
          entry->iterator->nextEntry();
        }
        // store min feature for term (for this shard; will later be merged into corpus-wide Db)
        string minFeatKey(termData->term);
        minFeatKey.append(FeatureStore::MIN_FEAT_SUFFIX);
        store.putFeature((char*)minFeatKey.c_str(), minFeat, (int)ctf);

        // get and store shard df feature for term
        double shardDf = termData->corpus.documentCount;
        string dfFeatKey(termData->term);
        dfFeatKey.append(FeatureStore::SIZE_FEAT_SUFFIX);
        store.putFeature((char*)dfFeatKey.c_str(), shardDf, (int)ctf);

        // store sum f
        string featKey(termData->term);
        featKey.append(FeatureStore::FEAT_SUFFIX);
        store.putFeature((char*) featKey.c_str(), featSum, (int)ctf);

        // store sum f^2
        string squaredFeatKey(termData->term);
        squaredFeatKey.append(FeatureStore::SQUARED_FEAT_SUFFIX);
        store.putFeature((char*) squaredFeatKey.c_str(), squaredFeatSum, (int)ctf);

        iter->nextEntry();
      }
      delete iter;

    }
  } else if (strcmp(argv[1], "buildcorpus") == 0) {
    using namespace indri::collection;
    using namespace indri::index;

    string dbPath = params["db"];
    string indexstr = params["index"];

    FeatureStore store(dbPath, false);
    vector<Repository*> indexes;

    char mutableLine[indexstr.size() + 1];
    std::strcpy(mutableLine, indexstr.c_str());

    // get all shard indexes and add to vector
    for (char* value = std::strtok(mutableLine, ":"); 
        value != NULL; 
        value = std::strtok(NULL, ":")) {
      Repository* repo = new Repository();
      repo->openRead(value);
      indexes.push_back(repo);
    }

    // go through all indexes and collect ctf and df statistics.
    long totalTermCount = 0;
    long totalDocCount = 0;

    int idxCnt = 1;
    vector<Repository*>::iterator it;
    for (it = indexes.begin(); it != indexes.end(); ++it) {
      cout << "Starting index " << idxCnt++ << endl;

      // if it has more than one index, quit
      Repository::index_state state = (*it)->indexes();
      if (state->size() > 1) {
        cout << "Index has more than 1 part. Can't deal with this, man.";
        exit(EXIT_FAILURE);
      }
      Index* index = (*state)[0];
      DocListFileIterator* iter = index->docListFileIterator();
      iter->startIteration();

      // add the total term length of shard
      totalTermCount += index->termCount();
      // add the shard size (# of docs)
      totalDocCount += index->documentCount();

      int termCnt = 0;
      // go through all terms in the index and collect df/ctf
      while (!iter->finished()) {
        termCnt++;
        if(termCnt % 100000 == 0) {
          cout << "  Finished " << termCnt << " terms" << endl;
        }

        DocListFileIterator::DocListData* entry = iter->currentEntry();
        TermData* termData = entry->termData;
        double ctf = termData->corpus.totalCount;
        double df = termData->corpus.documentCount;

        // this seems pointless, but if I don't do this, it crashes.
        entry->iterator->startIteration();
        while( !entry->iterator->finished() ) {
          indri::index::DocListIterator::DocumentData* doc = entry->iterator->currentEntry();
          entry->iterator->nextEntry();
        }

        // store df feature for term
        string dfFeatKey(termData->term);
        dfFeatKey.append(FeatureStore::SIZE_FEAT_SUFFIX);
        store.addValFeature((char*)dfFeatKey.c_str(), df, (int)ctf);

        // store ctf feature for term
        string ctfFeatKey(termData->term);
        ctfFeatKey.append(FeatureStore::TERM_SIZE_FEAT_SUFFIX);
        store.addValFeature((char*)ctfFeatKey.c_str(), ctf, (int)ctf);

        iter->nextEntry();
      }
      delete iter;
    }

    // add collection global features needed for shard ranking
    string totalTermKey(FeatureStore::TERM_SIZE_FEAT_SUFFIX);
    store.putFeature((char*)totalTermKey.c_str(), totalTermCount, FeatureStore::FREQUENT_TERMS+1);
    string featSize(FeatureStore::SIZE_FEAT_SUFFIX);
    store.putFeature((char*)featSize.c_str(), totalDocCount, FeatureStore::FREQUENT_TERMS+1);

  } else if (strcmp(argv[1], "mergemin") == 0) {
    string dbstr = params["db"];
    string index = params["index"];

    // get list of shard statistic dbs
    vector<string> dbs;
    tokenize(dbstr, ":", &dbs);

    // open dbs for the corpus store and each shard store
    FeatureStore corpusStore(dbs[0], false);
    vector<FeatureStore*> stores;
    for (uint i = 1; i < dbs.size(); i++) {
      stores.push_back(new FeatureStore(dbs[i], true));
    }

    // iterate through the database for all terms and find gloabl min feature
    FeatureStore::TermIterator* termit = corpusStore.getTermIterator();
    while (!termit->finished()) {
      // get a stem and its ctf
      pair<string,double> termAndCtf = termit->currrentEntry();
      string stem = termAndCtf.first;
      double ctf = termAndCtf.second;

      // keep track of min feature
      double globalMin = DBL_MAX;
      string minFeatKey(stem);
      minFeatKey.append(FeatureStore::MIN_FEAT_SUFFIX);

      // for each shard, grab the share min feature from its stats db and find global min
      vector<FeatureStore*>::iterator it;
      for (it = stores.begin(); it != stores.end(); ++it) {
        double currMin;
        (*it)->getFeature((char*)minFeatKey.c_str(), &currMin);
        if (currMin < globalMin) {
          globalMin = currMin;
        }
      }

      // store min feature for term
      corpusStore.putFeature((char*)minFeatKey.c_str(), globalMin, (int)ctf);

      termit->nextTerm();
    }
    delete termit;

    vector<FeatureStore*>::iterator it;
    for (it = stores.begin(); it != stores.end(); ++it) {
      delete (*it);
    }

  } else if (strcmp(argv[1], "run") == 0) {
    using namespace indri::collection;

    string dbstr = params["db"];
    string index = params["index"];
    int n_c = atoi(params["n_c"].c_str());
    int numShards = atoi(params["numShards"].c_str());

    // get list of shard statistic dbs
    vector<string> dbs;
    tokenize(dbstr, ":", &dbs);

    // get indri index
    Repository repo;
    repo.openRead(index);

    // initialize Taily ranker
    ShardRanker ranker(dbs, &repo, numShards, n_c);

    // get query file
    char* queryFile = getOption(argv, argv + argc, "-q");
    ifstream qfile;
    qfile.open(queryFile);

    string line;
    if (qfile.is_open()) {

      while (getline(qfile, line)) {
        char mutableLine[line.size() + 1];
        std::strcpy(mutableLine, line.c_str());

        char* qnum = std::strtok(mutableLine, ":");
        char* query = std::strtok(NULL, ":");

        vector<pair<int, double> > ranking;
        ranker.rank(query, &ranking);

        cout << qnum << ":" << query << endl;
        for(int i = 0; i < ranking.size(); i++) {
          cout << ranking[i].first << " " << ranking[i].second << endl;
        }
      }
      qfile.close();
    }

  } else {
    std::cout << "Unrecognized option." << std::endl;
    string dbPath = params["db"];
    string indexPath = params["index"];

    // create and open the data store
    FeatureStore store(dbPath, true);
    
    double val;
    cout << "love ";
    store.getFeature((char*)"love#f", &val);
    cout << val << " " ;
    store.getFeature((char*)"love#f2", &val);
    cout << val << endl;

    cout << "and ";
    store.getFeature((char*)"and#f", &val);
    cout << val << " " ;
    store.getFeature((char*)"and#f2", &val);
    cout << val << endl;

    cout << "orange ";
    store.getFeature((char*)"orange#f", &val);
    cout << "f "<< val << " " ;
    store.getFeature((char*)"orange#d", &val);
    cout << "d "<< val << " " ;
    store.getFeature((char*)"orange#t", &val);
    cout << "t "<< val << " " ;
    store.getFeature((char*)"orange#f2", &val);
    cout << "f2 "<< val << endl;

    cout << "apple ";
    store.getFeature((char*)"apple#f", &val);
    cout << val << " " ;
    store.getFeature((char*)"apple#d", &val);
    cout << val << " " ;
    store.getFeature((char*)"apple#t", &val);
    cout << val << " " ;
    store.getFeature((char*)"apple#f2", &val);
    cout << val << endl;

    cout << "apple min ";
    store.getFeature((char*)"apple#m", &val);
    cout << val << endl ;
    store.getFeature((char*)"#d", &val);
    cout << "size " << val << " " ;
    store.getFeature((char*)"#t", &val);
    cout << "size " << val << endl;
  }

  return EXIT_SUCCESS;
}
