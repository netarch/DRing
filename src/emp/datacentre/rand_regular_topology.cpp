#include "rand_regular_topology.h"
#include <vector>
#include <map>
#include <fstream>
#include <algorithm>
#include <string>
//#include "string.h"
#include <sstream>
#include <climits>
#include <strstream>
#include <iostream>
#include <stdlib.h>
#include <queue>
#include <set>
#include "gurobi_c++.h"
#include "main.h"
#include "ecnqueue.h"
#include "YenTopKShortestPathsAlg.h"
#include "BhandariTopKDisjointPathsAlg.h"


const FIND_PATH_ALGORITHM find_path_alg = SHORTEST2; // FIRST_HOP_INDIRECTION; //KDISJOINT; //ECMP; //KSHORT;

const double LAMBDA = 10000000; //0.5; //INF

extern uint32_t RTT;

string ntoa(double n);
string itoa(uint64_t n);

extern int N;

RandRegularTopology::RandRegularTopology(Logfile* lg, EventList* ev, string graphFile, queue_type qt){
  logfile = lg;
  eventlist = ev;
  qtype = qt;
  
  for (int i=0; i < NSW; i++)
    adjMatrix[i] = new vector<int>();

  //< read graph from the graphFile
  ifstream myfile(graphFile.c_str());
  string line;
  if (myfile.is_open()){
    while(myfile.good()){
	getline(myfile, line);
	if (line.find("->") == string::npos) break;
	int from = atoi(line.substr(0, line.find("->")).c_str());
	int to = atoi(line.substr(line.find("->") + 2).c_str());
    if(from >= NSW || to >= NSW){
        cout<<"Graph file has out of bounds nodes, "<<from<<"->"<<to<<", NSW: "<<NSW<<endl;
        exit(0);
    }
    //cout<<"(from, to, NSW): "<<from<<", "<<to<<", "<<NSW<<endl;
	adjMatrix[from]->push_back(to);
	adjMatrix[to]->push_back(from);
    }
    myfile.close();
  }
  //>

  // Ankit: Check adjacency matrix
  for (unsigned int i = 0; i < NSW; i++){
    cout << "SW " << i << " |NBRS| = "<<adjMatrix[i]->size()<<" NBRS = ";
    for (unsigned int j = 0; j < adjMatrix[i]->size(); j++)
      cout << (*adjMatrix[i])[j] << " ";
    cout << endl;
  }


  //< initialize a graph data structure for shortest paths algo
  string command = "sed 's/->/ /g' " + graphFile + " > temp_graph";

  cout<<"command: "<<command<<endl;

  int sysReturn = system(command.c_str());
  if (sysReturn != 0) {
	  cout << "ERROR: System command to process graph file failed" << endl;
  }
  myGraph = new Graph("temp_graph");
  //>
 
  cout<<"GraphFile: "<<graphFile<<endl;

  init_network();
  cout<< "Init network finished "<<endl;

  //compute all pair shortest paths
  floydWarshall();


}

void RandRegularTopology::init_network(){
  QueueLoggerSampling* queueLogger;

  cout<<"SVRPORTS: "<<SVRPORTS<<endl;

  // sw-svr
  for (int j=0;j<NSW;j++)
    for (int k=0;k<SVRPORTS;k++){
      queues_sw_svr[j][k] = NULL;
      pipes_sw_svr[j][k] = NULL;
      queues_svr_sw[j][k] = NULL;
      pipes_svr_sw[j][k] = NULL;
    }
  
  // sw-sw
  for (int j=0;j<NSW;j++)
    for (int k=0;k<NSW;k++){
      queues_sw_sw[j][k] = NULL;
      pipes_sw_sw[j][k] = NULL;
    }

  cout<<"init_network finished. Link speed: "<<speedFromPktps(HOST_NIC)<<endl;

   int logger_period_ms = 1000000;
   mem_b queue_size = SWITCH_BUFFER * Packet::data_packet_size();
   int svrcnt = 0;
   // sw-svr
   for (int j = 0; j < NSW; j++) {
	int nsvrports = K - adjMatrix[j]->size();
        for (int k = 0; k < nsvrports * OVERSUBSCRIPTION; k++) {
          // Downlink: sw to server = sw-svr
          svrcnt++;
          //printf("Downlink: sw[%d] to serverport[%d] = sw-svr \n", j, k);
          queueLogger = new QueueLoggerSampling(timeFromMs(logger_period_ms), *eventlist);
          logfile->addLogger(*queueLogger);

          queues_sw_svr[j][k] = alloc_queue(queueLogger, HOST_NIC, queue_size); //new RandomQueue(speedFromPktps(HOST_NIC), memFromPkt(SWITCH_BUFFER + RANDOM_BUFFER), *eventlist, queueLogger, memFromPkt(RANDOM_BUFFER));
          queues_sw_svr[j][k]->setName("SW_" + ntoa(j) + "-" + "DST_" +ntoa(k));
          logfile->writeName(*(queues_sw_svr[j][k]));

          pipes_sw_svr[j][k] = new Pipe(timeFromUs(RTT), *eventlist);
          pipes_sw_svr[j][k]->setName("Pipe-sw-svr-" + ntoa(j) + "-" + ntoa(k));
          logfile->writeName(*(pipes_sw_svr[j][k]));

          // Uplink: server to sw = svr-sw
          queueLogger = new QueueLoggerSampling(timeFromMs(logger_period_ms), *eventlist);
          logfile->addLogger(*queueLogger);

          queues_svr_sw[j][k] = alloc_queue(queueLogger, HOST_NIC, queue_size); //new RandomQueue(speedFromPktps(HOST_NIC), memFromPkt(SWITCH_BUFFER + RANDOM_BUFFER), *eventlist, queueLogger, memFromPkt(RANDOM_BUFFER));
          queues_svr_sw[j][k]->setName("SRC_" + ntoa(k) + "-" + "SW_" +ntoa(j));
          logfile->writeName(*(queues_svr_sw[j][k]));

          pipes_svr_sw[j][k] = new Pipe(timeFromUs(RTT), *eventlist);
          pipes_svr_sw[j][k]->setName("Pipe-svr-sw-" + ntoa(k) + "-" + ntoa(j));
          logfile->writeName(*(pipes_svr_sw[j][k]));
          //cout<<"done 2 "<<svrcnt<<" "<<K<<" "<<nsvrports<<" "<<OVERSUBSCRIPTION<<" "<<j<<" "<<adjMatrix[j]->size()<<endl;
        }
    }

  // Now use adjMatrix to add pipes etc. between switches
  for (int i = 0; i < NSW; i++) { // over the adjMatrix i.e. different switches
    for (unsigned int j = 0; j < adjMatrix[i]->size(); j++) { // over connections from each switch
	    
      int k = (*adjMatrix[i])[j];
      if ( i > k) continue;
      
      queueLogger = new QueueLoggerSampling(timeFromMs(logger_period_ms), *eventlist);
      logfile->addLogger(*queueLogger);

      queues_sw_sw[i][k] = alloc_queue(queueLogger, HOST_NIC, queue_size); //new RandomQueue(speedFromPktps(HOST_NIC), memFromPkt(SWITCH_BUFFER + RANDOM_BUFFER), *eventlist, queueLogger, memFromPkt(RANDOM_BUFFER));
      queues_sw_sw[i][k]->setName("SW_" + ntoa(i) + "-" + "SW_" +ntoa(k));
      logfile->writeName(*(queues_sw_sw[i][k]));

      pipes_sw_sw[i][k] = new Pipe(timeFromUs(RTT), *eventlist);
      pipes_sw_sw[i][k]->setName("Pipe-sw-sw-" + ntoa(i) + "-" + ntoa(k));
      logfile->writeName(*(pipes_sw_sw[i][k]));
	  
      queueLogger = new QueueLoggerSampling(timeFromMs(logger_period_ms), *eventlist);
      logfile->addLogger(*queueLogger);

      queues_sw_sw[k][i] = alloc_queue(queueLogger, HOST_NIC, queue_size); //new RandomQueue(speedFromPktps(HOST_NIC), memFromPkt(SWITCH_BUFFER + RANDOM_BUFFER), *eventlist, queueLogger, memFromPkt(RANDOM_BUFFER));
      queues_sw_sw[k][i]->setName("SW_" + ntoa(k) + "-" + "SW_" +ntoa(i));
      logfile->writeName(*(queues_sw_sw[k][i]));
	  
      pipes_sw_sw[k][i] = new Pipe(timeFromUs(RTT), *eventlist);
      pipes_sw_sw[k][i]->setName("Pipe-sw-sw-" + ntoa(k) + "-" + ntoa(i));
      logfile->writeName(*(pipes_sw_sw[k][i]));
    }
  }
}

void check_non_null(route_t* rt){
  int fail = 0;
  for (unsigned int i=1;i<rt->size()-1;i+=2)
    if (rt->at(i)==NULL){
      fail = 1;
      printf("Failed at %d/%d \n", i, rt->size()-1);
      break;
    }
  
  if (fail){
    //    cout <<"Null queue in route"<<endl;
    for (unsigned int i=1;i<rt->size()-1;i+=2)
      printf("%p ",rt->at(i));

    cout<<endl;
    assert(0);
  }
}

unsigned int RandRegularTopology::ConvertHostToSwitchPort(int host)
{
  //cout<<"ConvertHostToSwitchPort: "<<host<<" : "<<NSW<<" : "<<NHOST<<" : "<< OVERSUBSCRIPTION<<" : "<<SVRPORTS<<endl;
  int nsw_less_svrport = NSW - (NHOST/OVERSUBSCRIPTION)%NSW;  // The number of switches with less servers than others = (# of hosts % # of switches)
  unsigned int myportindex;
  if(HETERO==1){
	  if(SVRPORTS != 1){
		 int reducedSvrport = SVRPORTS - OVERSUBSCRIPTION;
		  if(host < nsw_less_svrport * reducedSvrport)
			  myportindex = host % reducedSvrport;
		  else
			  myportindex = (host - nsw_less_svrport * reducedSvrport) % (SVRPORTS);
	  }
	  else {
                  myportindex = 0;
	  }
  }
  else{
	  myportindex = host % SVRPORTS;
  }
  return myportindex;
}



int RandRegularTopology::ConvertHostToSwitch(int host)
{	
  //cout<<"ConvertHostToSwitch: "<<host<<" : "<<NSW<<" : "<<NHOST<<" : "<< OVERSUBSCRIPTION<<" : "<<SVRPORTS<<endl;
  int nsw_less_svrport = NSW - (NHOST/OVERSUBSCRIPTION)%NSW;  // The number of switches with less servers than others = (# of hosts % # of switches)
  int myswitchindex;
  if(HETERO==1){
	  if(SVRPORTS != 1){
		 int reducedSvrport = SVRPORTS - OVERSUBSCRIPTION;
		  if(host < nsw_less_svrport * reducedSvrport)
			  myswitchindex = host / reducedSvrport;
		  else
			  myswitchindex = (host - nsw_less_svrport * reducedSvrport) / (SVRPORTS) + nsw_less_svrport;
	  }
	  else {
		  myswitchindex =  nsw_less_svrport + host;
	  }
  }
  else{
	  myswitchindex = host / SVRPORTS;
  }
  return myswitchindex;
}


int RandRegularTopology::get_number_of_hosts(int torSwitch){
  int nsw_less_svrport = NSW - (NHOST/OVERSUBSCRIPTION)%NSW;  // The number of switches with less servers than others = (# of hosts % # of switches)
  if(HETERO==1){
    int reducedSvrport = SVRPORTS - OVERSUBSCRIPTION;
    if(torSwitch < nsw_less_svrport)
	    return reducedSvrport;
    else
	    return SVRPORTS;
  }
  else{
      return SVRPORTS;
  }
}

int RandRegularTopology::get_distance(int src, int dest){

  int src_sw = ConvertHostToSwitch(src);
  int dest_sw = ConvertHostToSwitch(dest);
  //std::cout<<"ShortestPathLen: "<<src<<"("<<src_sw<<")->"<<dest<<"("<<dest_sw<<") : "<<shortestPathLen[src_sw][dest_sw]<<endl;
  return shortestPathLen[src_sw][dest_sw];
}



pair<vector<double>*, vector<route_t*>*> RandRegularTopology::get_paths(int src, int dest){
   return get_paths_helper(src, dest, find_path_alg);
}


pair<vector<double>*, vector<route_t*>*> RandRegularTopology::get_other_paths(int src, int dest){
   return get_paths_helper(src, dest, KSHORT);
}

pair<vector<double>*, vector<route_t*>*> RandRegularTopology::get_paths_helper(int src, int dest, FIND_PATH_ALGORITHM find_path_alg){

  if(pathcache.find(pair<int, int>(src, dest)) != pathcache.end())
    return pathcache[pair<int, int>(src, dest)];

  vector<route_t*>* paths = new vector<route_t*>();
  vector<double>* pathweights = NULL;

  route_t* routeout;

  //for(int z=0; z<NHOST; z++)
  //      cout << "Host " << z << " Connect to switch " << ConvertHostToSwitch(z) << endl;

 


  // Put Yen algorithm's src-dest shortest paths in routeout

  //cout << "HERETO " << HETERO << " NHOST " << NHOST << " NSW " << NSW << " SVRPORTS " << SVRPORTS << endl;

  int src_sw = ConvertHostToSwitch(src);
  int dest_sw = ConvertHostToSwitch(dest);

  // Ankit: Testing if our numbering of switches/servers and topology construction is causing issues
  //cout << " From Switch " << src_sw << " to switch " << dest_sw << endl;

  //< If same switch then only path through switch
  if (src_sw == dest_sw){
    Queue* pqueue = new Queue(speedFromPktps(HOST_NIC), memFromPkt(FEEDER_BUFFER), *eventlist, NULL);
    pqueue->setName("PQueue_" + ntoa(src) + "_" + ntoa(dest));
  
    routeout = new route_t();
    routeout->push_back(pqueue);

    routeout->push_back(queues_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);
    routeout->push_back(pipes_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);

    routeout->push_back(queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);
    routeout->push_back(pipes_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);

    paths->push_back(routeout);

    //cout << "CHECK-NOT-NULL AT SAME SWITCH SERVERS" << endl;
    check_non_null(routeout);
    return pair<vector<double>*, vector<route_t*>*>(pathweights, paths);
  }
  //>

  else { 
      if(find_path_alg == KSHORT){
          // Use the shortest path algo to set this stuff up
          YenTopKShortestPathsAlg yenAlg(myGraph, myGraph->get_vertex(src_sw), myGraph->get_vertex(dest_sw));

          int i=0;
          int shortestLen = -1;
          int numpaths = NUMPATHS;
          if(shortestPathLen[src_sw][dest_sw] == 1) numpaths = 6;
          else if(shortestPathLen[src_sw][dest_sw] == 2) numpaths = 6;
          else if(shortestPathLen[src_sw][dest_sw] == 3) numpaths = 6;
          else if(shortestPathLen[src_sw][dest_sw] == 4) numpaths = 6;
          //printf("[%d --> %d] dist: %d, numpaths: %d \n", src_sw, dest_sw, shortestPathLen[src_sw][dest_sw], numpaths);
          while(yenAlg.has_next() && i < numpaths){
              // Ankit: Checking if YenAlgo gives anything
              //cout << "YEN-ALGO gave some paths" << endl;

              ++i;                                                                                                           

              vector<BaseVertex*> pathIHave = yenAlg.next()->m_vtVertexList;

              //	if (shortestLen == -1) shortestLen = pathIHave.size();
              //	if (pathIHave.size() > shortestLen + 1) break;

              Queue* pqueue = new Queue(speedFromPktps(HOST_NIC), memFromPkt(FEEDER_BUFFER), *eventlist, NULL);
              pqueue->setName("PQueue_" + ntoa(src) + "_" + ntoa(dest));

              routeout = new route_t();
              routeout->push_back(pqueue);

              // First hop = svr to sw
              routeout->push_back(queues_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);
              routeout->push_back(pipes_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);

				  // Ankit: Print path given by Yen algo
				  //cout << "YEN PATH NEW = ";
				  static int num_fail = 0;
				  static int total_paths = 0;
				  total_paths++;
				  int intermediate_switch = -1;
				  for (unsigned int hop = 1; hop < pathIHave.size(); hop++) {
					  int fr = pathIHave[hop-1]->getID();
					  int to = pathIHave[hop]->getID();

					  if(shortestPathLen[src_sw][to] == hop && shortestPathLen[to][dest_sw] == pathIHave.size()-1-hop)
						  intermediate_switch = to;

					  //cout << fr << " -> " << to << " -> ";

					  routeout->push_back(queues_sw_sw[fr][to]);
					  //cout << "(Converted = " << queues_sw_sw[fr][to]->str() << ")";
					  routeout->push_back(pipes_sw_sw[fr][to]);
				  }

				  cout<<"Path ["<<src_sw<<"-->"<<dest_sw<<"] Intermediate: ";
				  if(intermediate_switch == -1)
				  {	cout<<"NULL"; num_fail++; }
				  else
					  cout<<intermediate_switch;
				  cout<<"(Failed: "<<num_fail<<"/"<<total_paths<<")"<<endl;

				  //cout << endl;

              //cout << "(Add final = " << dest << "(" << dest_sw << ") : "<< ConvertHostToSwitchPort(dest) <<" : "<<queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]<<endl;

              routeout->push_back(queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);
              routeout->push_back(pipes_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);

              paths->push_back(routeout);
              //cout << "CHECK-NOT-NULL AT DIFFERENT SWITCH" << endl;
              check_non_null(routeout);
          }

          yenAlg.clear();
          pathcache[pair<int, int>(src, dest)] = pair<vector<double>*, vector<route_t*>*> (pathweights, paths);
    	  return pair<vector<double>*, vector<route_t*>*>(pathweights, paths);
          //return paths;
      }

      else if(find_path_alg == KDISJOINT){
          // Set this stuff up, why do you need to specify source and vertex?
          BhandariTopKDisjointPathsAlg BhandariAlg(myGraph, myGraph->get_vertex(src_sw), myGraph->get_vertex(dest_sw));
          int i=0;
          int shortestLen = -1;
          vector<BasePath*> pathsIHave;
          int numpaths = NUMPATHS;
          if(shortestPathLen[src_sw][dest_sw] == 1) numpaths = 8;
          else if(shortestPathLen[src_sw][dest_sw] == 2) numpaths = 8;
          else if(shortestPathLen[src_sw][dest_sw] == 3) numpaths = 8;
          else if(shortestPathLen[src_sw][dest_sw] == 4) numpaths = 8;
          //printf("[%d --> %d] dist: %d, numpaths: %d \n", src_sw, dest_sw, shortestPathLen[src_sw][dest_sw], numpaths);
          BhandariAlg.KDisjointPaths(numpaths, pathsIHave);
          for(int i=0; i<pathsIHave.size(); i++){
              //Vipul: Checking if BhandariAlgo gives anything
              //cout << "Bhandari-ALGO gave some paths" << endl;

              vector<BaseVertex*> pathIHave = pathsIHave[i]->m_vtVertexList;;

              //	if (shortestLen == -1) shortestLen = pathIHave.size();
              //	if (pathIHave.size() > shortestLen + 1) break;

              Queue* pqueue = new Queue(speedFromPktps(HOST_NIC), memFromPkt(FEEDER_BUFFER), *eventlist, NULL);
              pqueue->setName("PQueue_" + ntoa(src) + "_" + ntoa(dest));

              routeout = new route_t();
              routeout->push_back(pqueue);

              // First hop = svr to sw
              routeout->push_back(queues_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);
              routeout->push_back(pipes_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);
      
				  // Vipul: Print path given by Bhandari algo
				  //cout << "Bhandari PATH NEW = ";
				  for (unsigned int hop = 1; hop < pathIHave.size(); hop++) {
					  int fr = pathIHave[hop-1]->getID();
					  int to = pathIHave[hop]->getID();
					  //cout << fr << " -> " << to << " -> ";
					  routeout->push_back(queues_sw_sw[fr][to]);
					  //cout << "(Converted = " << queues_sw_sw[fr][to]->str() << ")";
					  routeout->push_back(pipes_sw_sw[fr][to]);
				  }
				  //cout << endl;
				  /*
					  static int num_fail = 0; 
					  static int total_paths = 0;
					  total_paths++;
					  cout<<"KDisjoint Path ["<<src_sw<<"-->"<<dest_sw<<"] Intermediate: ";
					  if(intermediate_switch == -1)
					  {	cout<<"NULL"<<endl; num_fail++; }
					  else
					  cout<<intermediate_switch<<endl;
					  cout<<"(Failed: "<<num_fail<<"/"<<total_paths<<")"<<endl;
					*/ 
              routeout->push_back(queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);
              routeout->push_back(pipes_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);
              //cout << "(Add final = " << dest_sw << ": "<< ConvertHostToSwitchPort(dest) <<" : "<<queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]<<endl;
              paths->push_back(routeout);
              //cout << "CHECK-NOT-NULL AT DIFFERENT SWITCH" << endl;
              check_non_null(routeout);
          }

          BhandariAlg.clear();
          pathcache[pair<int, int>(src, dest)] = pair<vector<double>*, vector<route_t*>*> (pathweights, paths);
    	    return pair<vector<double>*, vector<route_t*>*>(pathweights, paths);
          //return paths;
      }
      else if(find_path_alg == SHORTEST2){

			//return all shortest paths	 
			vector<vector<BaseVertex* > > shortest_paths;
			queue<vector<BaseVertex*> > shortest_paths_till_now;

			vector<BaseVertex*> path_till_now;
			path_till_now.push_back(myGraph->get_vertex(src_sw));
			shortest_paths_till_now.push(path_till_now);

			while(!shortest_paths_till_now.empty()){
				vector<BaseVertex*> path_till_now = shortest_paths_till_now.front();
				shortest_paths_till_now.pop();
				BaseVertex* last_vertex = path_till_now.back();
				set<BaseVertex*> possible_next_hops;
				myGraph->get_adjacent_vertices(last_vertex, possible_next_hops);
				for(BaseVertex* next_hop: possible_next_hops){
					if(next_hop == myGraph->get_vertex(dest_sw)){
						//found a shortest path!
						vector<BaseVertex*> shortest_path(path_till_now);
						shortest_path.push_back(next_hop);
						shortest_paths.push_back(shortest_path);
					}
					else if (path_till_now.size() <= 1){
                        bool new_hop = true;
                        for (BaseVertex* path_vertex: path_till_now){
                            new_hop = new_hop and (path_vertex->getID() != next_hop->getID());
                        }
                        if (new_hop){
                            vector<BaseVertex*> shortest_path_till_now(path_till_now);
                            shortest_path_till_now.push_back(next_hop);
                            shortest_paths_till_now.push(shortest_path_till_now);
                        }
					}
                    //evaluate this case only for shortest paths
					else if(shortestPathLen[src_sw][dest_sw] == (path_till_now.size() - 1)
                                                              + shortestPathLen[last_vertex->getID()][dest_sw] &&
                            shortestPathLen[last_vertex->getID()][dest_sw] == 1 + shortestPathLen[next_hop->getID()][dest_sw]){
                        vector<BaseVertex*> shortest_path_till_now(path_till_now);
						shortest_path_till_now.push_back(next_hop);
						shortest_paths_till_now.push(shortest_path_till_now);
					}
				}
			}

			int i=0;
			//printf("[%d --> %d] dist: %d, numpaths: %d \n", src_sw, dest_sw, shortestPathLen[src_sw][dest_sw], numpaths);
			while(i < shortest_paths.size()){
				// Ankit: Checking if YenAlgo gives anything
				//cout << "SHORTEST_PATH-ALGO gave some paths" << endl;

				vector<BaseVertex*> pathIHave = shortest_paths[i];

				//	if (shortestLen == -1) shortestLen = pathIHave.size();
				//	if (pathIHave.size() > shortestLen + 1) break;

				Queue* pqueue = new Queue(speedFromPktps(HOST_NIC), memFromPkt(FEEDER_BUFFER), *eventlist, NULL);
				pqueue->setName("PQueue_" + ntoa(src) + "_" + ntoa(dest));

				routeout = new route_t();
				routeout->push_back(pqueue);

				// First hop = svr to sw
				routeout->push_back(queues_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);
				routeout->push_back(pipes_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);

				// Ankit: Print path given by All Shortest paths
				//cout << "SHORTEST PATH NEW = ";
				for (unsigned int hop = 1; hop < pathIHave.size(); hop++) {
					int fr = pathIHave[hop-1]->getID();
					int to = pathIHave[hop]->getID();

					//cout << fr << " -> " << to << " -> ";
					routeout->push_back(queues_sw_sw[fr][to]);
					//cout << "(Converted = " << queues_sw_sw[fr][to]->str() << ")";
					routeout->push_back(pipes_sw_sw[fr][to]);
				}


				//cout << endl;
				//cout << "(Add final = " << dest << "(" << dest_sw << ") : "<< ConvertHostToSwitchPort(dest) <<" : "<<queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]<<endl;
				routeout->push_back(queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);
				routeout->push_back(pipes_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);

				paths->push_back(routeout);
				//cout << "CHECK-NOT-NULL AT DIFFERENT SWITCH" << endl;
				check_non_null(routeout);
				++i;                                                                                                           
			}
			pathcache[pair<int, int>(src, dest)] = pair<vector<double>*, vector<route_t*>*> (pathweights, paths);
			return pair<vector<double>*, vector<route_t*>*>(pathweights, paths);
			//return paths;
      }
      else if(find_path_alg == FIRST_HOP_INDIRECTION){

			//return all shortest paths	 
			vector<vector<BaseVertex* > > shortest_paths;
			queue<vector<BaseVertex*> > shortest_paths_till_now;

			vector<BaseVertex*> path_till_now;
			path_till_now.push_back(myGraph->get_vertex(src_sw));
            set<BaseVertex*> possible_first_hops;
            myGraph->get_adjacent_vertices(myGraph->get_vertex(src_sw), possible_first_hops);

            /*
            int src_dest_dist = shortestPathLen[src_sw][dest_sw];
            for(BaseVertex* next_hop: possible_first_hops){
                if(next_hop == myGraph->get_vertex(dest_sw)){
                    //destination is a neighbour, skip the direct path
                    src_dest_dist += 1;
                }
            }
            */
            for(BaseVertex* next_hop: possible_first_hops){
                if(next_hop == myGraph->get_vertex(dest_sw)){
                    vector<BaseVertex*> shortest_path(path_till_now);
                    shortest_path.push_back(next_hop);
                    shortest_paths.push_back(shortest_path);
                }
                else{ //if(shortestPathLen[next_hop->getID()][dest_sw] <= src_dest_dist){
                    vector<BaseVertex*> shortest_path_till_now(path_till_now);
                    shortest_path_till_now.push_back(next_hop);
                    shortest_paths_till_now.push(shortest_path_till_now);
                }
            }

			while(!shortest_paths_till_now.empty()){
				vector<BaseVertex*> path_till_now = shortest_paths_till_now.front();
				shortest_paths_till_now.pop();
				BaseVertex* last_vertex = path_till_now.back();
				set<BaseVertex*> possible_next_hops;
				myGraph->get_adjacent_vertices(last_vertex, possible_next_hops);
				for(BaseVertex* next_hop: possible_next_hops){
					if(next_hop == myGraph->get_vertex(dest_sw)){
						//found a shortest path!
						vector<BaseVertex*> shortest_path(path_till_now);
						shortest_path.push_back(next_hop);
						shortest_paths.push_back(shortest_path);
					}
					else if(shortestPathLen[last_vertex->getID()][dest_sw] == 1 + shortestPathLen[next_hop->getID()][dest_sw]){
						vector<BaseVertex*> shortest_path_till_now(path_till_now);
						shortest_path_till_now.push_back(next_hop);
						shortest_paths_till_now.push(shortest_path_till_now);
					}
				}
			}

			int i=0;
			//printf("[%d --> %d] dist: %d, numpaths: %d \n", src_sw, dest_sw, shortestPathLen[src_sw][dest_sw], numpaths);
			while(i < shortest_paths.size()){
				// Ankit: Checking if YenAlgo gives anything
				//cout << "SHORTEST_PATH-ALGO gave some paths" << endl;

				vector<BaseVertex*> pathIHave = shortest_paths[i];

				//	if (shortestLen == -1) shortestLen = pathIHave.size();
				//	if (pathIHave.size() > shortestLen + 1) break;

				Queue* pqueue = new Queue(speedFromPktps(HOST_NIC), memFromPkt(FEEDER_BUFFER), *eventlist, NULL);
				pqueue->setName("PQueue_" + ntoa(src) + "_" + ntoa(dest));

				routeout = new route_t();
				routeout->push_back(pqueue);

				// First hop = svr to sw
				routeout->push_back(queues_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);
				routeout->push_back(pipes_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);

				// Ankit: Print path given by All Shortest paths
				//cout << "SHORTEST PATH NEW = ";
				for (unsigned int hop = 1; hop < pathIHave.size(); hop++) {
					int fr = pathIHave[hop-1]->getID();
					int to = pathIHave[hop]->getID();

					//cout << fr << " -> " << to << " -> ";
					routeout->push_back(queues_sw_sw[fr][to]);
					//cout << "(Converted = " << queues_sw_sw[fr][to]->str() << ")";
					routeout->push_back(pipes_sw_sw[fr][to]);
				}


				//cout << endl;
				//cout << "(Add final = " << dest << "(" << dest_sw << ") : "<< ConvertHostToSwitchPort(dest) <<" : "<<queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]<<endl;
				routeout->push_back(queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);
				routeout->push_back(pipes_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);

				paths->push_back(routeout);
				//cout << "CHECK-NOT-NULL AT DIFFERENT SWITCH" << endl;
				check_non_null(routeout);
				++i;                                                                                                           
			}
			pathcache[pair<int, int>(src, dest)] = pair<vector<double>*, vector<route_t*>*> (pathweights, paths);
			return pair<vector<double>*, vector<route_t*>*>(pathweights, paths);
			//return paths;
      }
      else if(find_path_alg == ECMP){

			//return all shortest paths	 
			vector<vector<BaseVertex* > > shortest_paths;
			queue<vector<BaseVertex*> > shortest_paths_till_now;

			vector<BaseVertex*> path_till_now;
			path_till_now.push_back(myGraph->get_vertex(src_sw));
			shortest_paths_till_now.push(path_till_now);

			while(!shortest_paths_till_now.empty()){
				vector<BaseVertex*> path_till_now = shortest_paths_till_now.front();
				shortest_paths_till_now.pop();
				BaseVertex* last_vertex = path_till_now.back();
				set<BaseVertex*> possible_next_hops;
				myGraph->get_adjacent_vertices(last_vertex, possible_next_hops);
				for(BaseVertex* next_hop: possible_next_hops){
					if(next_hop == myGraph->get_vertex(dest_sw)){
						//found a shortest path!
						vector<BaseVertex*> shortest_path(path_till_now);
						shortest_path.push_back(next_hop);
						shortest_paths.push_back(shortest_path);
					}
					else if(shortestPathLen[last_vertex->getID()][dest_sw] == 1 + shortestPathLen[next_hop->getID()][dest_sw]){
						vector<BaseVertex*> shortest_path_till_now(path_till_now);
						shortest_path_till_now.push_back(next_hop);
						shortest_paths_till_now.push(shortest_path_till_now);
					}
				}
			}

			int i=0;
			//printf("[%d --> %d] dist: %d, numpaths: %d \n", src_sw, dest_sw, shortestPathLen[src_sw][dest_sw], numpaths);
			while(i < shortest_paths.size()){
				// Ankit: Checking if YenAlgo gives anything
				//cout << "SHORTEST_PATH-ALGO gave some paths" << endl;

				vector<BaseVertex*> pathIHave = shortest_paths[i];

				//	if (shortestLen == -1) shortestLen = pathIHave.size();
				//	if (pathIHave.size() > shortestLen + 1) break;

				Queue* pqueue = new Queue(speedFromPktps(HOST_NIC), memFromPkt(FEEDER_BUFFER), *eventlist, NULL);
				pqueue->setName("PQueue_" + ntoa(src) + "_" + ntoa(dest));

				routeout = new route_t();
				routeout->push_back(pqueue);

				// First hop = svr to sw
				routeout->push_back(queues_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);
				routeout->push_back(pipes_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);

				// Ankit: Print path given by All Shortest paths
				//cout << "SHORTEST PATH NEW = ";
				for (unsigned int hop = 1; hop < pathIHave.size(); hop++) {
					int fr = pathIHave[hop-1]->getID();
					int to = pathIHave[hop]->getID();

					//cout << fr << " -> " << to << " -> ";
					routeout->push_back(queues_sw_sw[fr][to]);
					//cout << "(Converted = " << queues_sw_sw[fr][to]->str() << ")";
					routeout->push_back(pipes_sw_sw[fr][to]);
				}


				//cout << endl;
				//cout << "(Add final = " << dest << "(" << dest_sw << ") : "<< ConvertHostToSwitchPort(dest) <<" : "<<queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]<<endl;
				routeout->push_back(queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);
				routeout->push_back(pipes_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);

				paths->push_back(routeout);
				//cout << "CHECK-NOT-NULL AT DIFFERENT SWITCH" << endl;
				check_non_null(routeout);
				++i;                                                                                                           
			}
			pathcache[pair<int, int>(src, dest)] = pair<vector<double>*, vector<route_t*>*> (pathweights, paths);
			return pair<vector<double>*, vector<route_t*>*>(pathweights, paths);
			//return paths;
      }
      else if(find_path_alg == ECMP_DAG){

			if(nextHops.find(dest_sw) == nextHops.end()){
				nextHops[dest_sw] = map<int, map<BaseVertex*, double> >();
				queue<BaseVertex*> bfsQueue;
				bfsQueue.push(myGraph->get_vertex(dest_sw));
				while(!bfsQueue.empty()){
					BaseVertex* node = bfsQueue.front();
					bfsQueue.pop();
					set<BaseVertex*> possible_next_hops;
					myGraph->get_adjacent_vertices(node, possible_next_hops);
					for(BaseVertex* next_hop: possible_next_hops){
						if(shortestPathLen[next_hop->getID()][dest_sw] == 1 + shortestPathLen[node->getID()][dest_sw]){
							if(nextHops[dest_sw].find(next_hop->getID()) == nextHops[dest_sw].end())
								bfsQueue.push(next_hop);
							nextHops[dest_sw][next_hop->getID()][node] = 1.0;
						}
					}
				}
				/* ECMP hops populated, now add some extra edges for ECMP */

				for(int k=4; k<=3; k++){
					set<int> nodesAtDistK;
					vector<BaseVertex*> allVertices(myGraph->get_allVertices().begin(), myGraph->get_allVertices().end());
					std::random_shuffle ( allVertices.begin(), allVertices.end() );  
					for (BaseVertex* v: allVertices){
						if(shortestPathLen[dest_sw][v->getID()] == k){
							nodesAtDistK.insert(v->getID());
						}
					}
					int numExtraEdges = 0;
					while(!nodesAtDistK.empty()){
						int node = *(nodesAtDistK.begin());
						nodesAtDistK.erase(node);
						set<BaseVertex*> possible_next_hops;
						myGraph->get_adjacent_vertices(myGraph->get_vertex(node), possible_next_hops);
						for(BaseVertex* next_hop: possible_next_hops){
							if(nodesAtDistK.find(next_hop->getID()) != nodesAtDistK.end()){
								nodesAtDistK.erase(next_hop->getID());
								nextHops[dest_sw][next_hop->getID()][myGraph->get_vertex(node)] = 0.5; //1.0/k;
								nextHops[dest_sw][node][next_hop] = 0.5; //1.0/k;
								numExtraEdges++;
								break;
							}
						}
					}
					cout<<"At dist: "<<k<<", Found "<<numExtraEdges<<" extra edges for dest: "<<dest_sw<<endl;
				}
			}


			pathweights = new vector<double>();

			//return all possible ecmp paths	 
			vector<vector<BaseVertex* > > ecmp_paths;
			queue<pair<vector<BaseVertex*>, double> > ecmp_paths_till_now;
			typedef pair<vector<BaseVertex*>, double> queueEntry;

			vector<BaseVertex*> path_till_now;
			path_till_now.push_back(myGraph->get_vertex(src_sw));
			ecmp_paths_till_now.push(queueEntry(path_till_now, 1.0));


			while(!ecmp_paths_till_now.empty()){
				queueEntry entry = ecmp_paths_till_now.front();
				vector<BaseVertex*> path_till_now = entry.first;
				double path_till_now_wt = entry.second;
				ecmp_paths_till_now.pop();
				BaseVertex* last_vertex = path_till_now.back();
				map<BaseVertex*, double> next_hops = nextHops[dest_sw][last_vertex->getID()];
				double total_wt = 0.0;
				for(auto& next_hop_entry: next_hops){
					BaseVertex* next_hop = next_hop_entry.first;
					double next_hop_wt = next_hop_entry.second;
					if(next_hop == myGraph->get_vertex(dest_sw) || path_till_now.size()<=1 || path_till_now.rbegin()[1]->getID() != next_hop->getID()){
						total_wt += next_hop_wt;
					}
				}
				for(auto& next_hop_entry: next_hops){
					BaseVertex* next_hop = next_hop_entry.first;
					double next_hop_wt = next_hop_entry.second;
					if(next_hop == myGraph->get_vertex(dest_sw)){
						//found a shortest path!
						vector<BaseVertex*> ecmp_path(path_till_now);
						ecmp_path.push_back(next_hop);
						ecmp_paths.push_back(ecmp_path);
						pathweights->push_back((path_till_now_wt * next_hop_wt)/total_wt);
					}
					else if(path_till_now.size()<=1 || path_till_now.rbegin()[1]->getID() != next_hop->getID()){
						//assert(shortestPathLen[last_vertex->getID()][dest_sw] == 1 + shortestPathLen[next_hop->getID()][dest_sw]);
						vector<BaseVertex*> ecmp_path_till_now(path_till_now);
						ecmp_path_till_now.push_back(next_hop);
						ecmp_paths_till_now.push(queueEntry(ecmp_path_till_now, (path_till_now_wt * next_hop_wt)/total_wt));
					}
				}
			}

			int i=0;
			printf("[%d --> %d] dist: %d, numpaths: %d \n", src_sw, dest_sw, shortestPathLen[src_sw][dest_sw], ecmp_paths.size());
			while(i < ecmp_paths.size()){
				// Ankit: Checking if YenAlgo gives anything
				//cout << "SHORTEST_PATH-ALGO gave some paths" << endl;

				vector<BaseVertex*> pathIHave = ecmp_paths[i];

				cout<<"Path of size: "<<pathIHave.size()-1<<" weight: "<<pathweights->at(i)<<endl;

				//	if (shortestLen == -1) shortestLen = pathIHave.size();
				//	if (pathIHave.size() > shortestLen + 1) break;

				Queue* pqueue = new Queue(speedFromPktps(HOST_NIC), memFromPkt(FEEDER_BUFFER), *eventlist, NULL);
				pqueue->setName("PQueue_" + ntoa(src) + "_" + ntoa(dest));

				routeout = new route_t();
				routeout->push_back(pqueue);

				// First hop = svr to sw
				routeout->push_back(queues_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);
				routeout->push_back(pipes_svr_sw[src_sw][ConvertHostToSwitchPort(src)]);

				// Ankit: Print path given by All Shortest paths
				//cout << "SHORTEST PATH NEW = ";
				for (unsigned int hop = 1; hop < pathIHave.size(); hop++) {
					int fr = pathIHave[hop-1]->getID();
					int to = pathIHave[hop]->getID();

					//cout << fr << " -> " << to << " -> ";
					routeout->push_back(queues_sw_sw[fr][to]);
					//cout << "(Converted = " << queues_sw_sw[fr][to]->str() << ")";
					routeout->push_back(pipes_sw_sw[fr][to]);
				}

				//cout << endl;
				//cout << "(Add final = " << dest << "(" << dest_sw << ") : "<< ConvertHostToSwitchPort(dest) <<" : "<<queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]<<endl;
				routeout->push_back(queues_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);
				routeout->push_back(pipes_sw_svr[dest_sw][ConvertHostToSwitchPort(dest)]);

				paths->push_back(routeout);
				//cout << "CHECK-NOT-NULL AT DIFFERENT SWITCH" << endl;
				check_non_null(routeout);
				++i;                                                                                                           
			}
			pathcache[pair<int, int>(src, dest)] = pair<vector<double>*, vector<route_t*>*> (pathweights, paths);
			return pair<vector<double>*, vector<route_t*>*>(pathweights, paths);
      }
  }
}

void RandRegularTopology::floydWarshall(){

	for(int i = 0; i < NSW; i++){
		for(int j = 0; j < NSW; j++){
			if(i == j)
				shortestPathLen[i][j] = 0;
			else
				shortestPathLen[i][j] = INT_MAX/2; //to avoid overflow
		}
		vector<int> *nbrs = adjMatrix[i];
		for (int nbr: *nbrs)
			shortestPathLen[i][nbr] = 1;
	}

	//floyd warshall
	for(int k = 0; k < NSW; k++){
		for(int i = 0; i < NSW; i++){
			for(int j = 0; j < NSW; j++){
				if(shortestPathLen[i][j] > shortestPathLen[i][k] + shortestPathLen[k][j])
					shortestPathLen[i][j] = shortestPathLen[i][k] + shortestPathLen[k][j];
			}
		}
	}
	for(int k = 0; k < NSW; k++){
		for(int i = 0; i < NSW; i++){
            if (shortestPathLen[k][i] > 2){
	            //cout << " Distance ("<< k << " " << i << ") " << shortestPathLen[k][i] << endl;
                //exit(0);
            }
		}
	}
}


int RandRegularTopology::getHostsInRack(int rack){
  int nsw_less_svrport = (NSW - ((NHOST/OVERSUBSCRIPTION)%NSW))%NSW;  // The number of switches with less servers than others = (# of hosts % # of switches)
  //cout<<"getHostsInRack: "<<rack<<" : "<<NSW<<" : "<<NHOST<<" : "<< OVERSUBSCRIPTION<<" : "<<SVRPORTS<<" : " << nsw_less_svrport << endl;
  int reducedSvrport = SVRPORTS - OVERSUBSCRIPTION;
  if(rack < nsw_less_svrport) return reducedSvrport;
  else return SVRPORTS;
}


pair<vector<int>, vector<int> > RandRegularTopology::getcsRacks(int clients, int servers){
   vector<int> clientracks, serverracks;
   vector<int> racks;
   for(int i=0; i<NSW; i++){
    racks.push_back(i);
   }
   std::random_shuffle(racks.begin(), racks.end());
   int cCovered=0, sCovered=0;
   int curr_rack_id=0;
   while(curr_rack_id < NSW and cCovered < clients){
    int rack = racks[curr_rack_id];
    cout<<"CClient rack: "<<rack << " " << cCovered << " " << clients <<endl;
    clientracks.push_back(rack);
    cCovered += getHostsInRack(rack);
    curr_rack_id++;
   }
   while(curr_rack_id < NSW and sCovered < servers){
    int rack = racks[curr_rack_id];
    cout<<"SServer rack: "<<rack<<endl;
    serverracks.push_back(rack);
    sCovered += getHostsInRack(rack);
    curr_rack_id++;
   }

   if (cCovered < clients or sCovered < servers){
    cout << "Not enough racks for C-S model traffic with C=" << clients << " S=" << servers << endl;
    exit(0);
   }

   return pair<vector<int>, vector<int> > (clientracks, serverracks);
}

/*
pair<vector<int>, vector<int> > RandRegularTopology::getcsRacks(int clients, int servers){
   int cracks = (clients * NSW + NHOST-1)/NHOST, sracks = (servers * NSW + NHOST-1)/NHOST;
   vector<int> clientracks, serverracks;

   try{
      GRBEnv env = GRBEnv();
      GRBModel model = GRBModel(env);
     
      GRBVar nodeC[NSW], nodeS[NSW], nodeF[NSW], nodeG[NSW];
  
      for(int i=0; i<NSW; i++){
         nodeC[i] = model.addVar(0.0, 1.0, 0.0, GRB_BINARY, "c_" + to_string(i));
         nodeS[i] = model.addVar(0.0, 1.0, 0.0, GRB_BINARY, "s_" + to_string(i));
         nodeF[i] = model.addVar(0.0, SVRPORTS, 1.0, GRB_CONTINUOUS, "f_" + to_string(i));
         nodeG[i] = model.addVar(0.0, SVRPORTS, 0.0, GRB_CONTINUOUS, "g_" + to_string(i));
      }

      cout<<"All nodevars creates"<<endl;

      vector<GRBVar*> edgeVars[NSW];
      GRBLinExpr cons[NSW], obj;
      const double *plusone = new double(1.0);
      const double *negone = new double(-1.0);
      for(int i=0; i<NSW; i++){
         edgeVars[i].resize(adjMatrix[i]->size());
         for(int j=0; j<adjMatrix[i]->size(); j++){
            int nbr = adjMatrix[i]->at(j);
            edgeVars[i][j] = new GRBVar(model.addVar(0.0, 1.0, 0.0, GRB_CONTINUOUS, "e_" + to_string(i) + "_" + to_string(nbr)));
            //cout<<i<<" "<<j<<" "<<nbr<<" : "<<edgeVars[i][j]<<endl;
            //model.addConstr(*edgeVars[i][j], GRB_LESS_EQUAL, 1.0, "edgecap_" + to_string(i) + "_" + to_string(nbr));
            cons[i].addTerms(plusone, edgeVars[i][j], 1);
            cons[nbr].addTerms(negone, edgeVars[i][j], 1);
         }
         cons[i].addTerms(plusone, &nodeF[i], 1);
         cons[i].addTerms(negone, &nodeG[i], 1);
      }

      cout<<"All edge vars created"<<endl;

      GRBLinExpr cCons, sCons;
      for(int i=0; i<NSW; i++){
         obj.addTerms(plusone, &nodeF[i], 1);
         cCons.addTerms(plusone, &nodeC[i], 1);
         sCons.addTerms(plusone, &nodeS[i], 1);
         model.addConstr(cons[i], GRB_EQUAL, 0, "preserve_" + to_string(i));
         model.addConstr(nodeC[i] + nodeS[i], GRB_LESS_EQUAL, 1.0, "disjoint_" + to_string(i));
         model.addConstr(nodeF[i] - SVRPORTS * nodeC[i], GRB_LESS_EQUAL, 0.0, "inrack_" + to_string(i));
         model.addConstr(nodeG[i] - SVRPORTS * nodeS[i], GRB_LESS_EQUAL, 0.0, "outrack_" + to_string(i));
      }

      model.addConstr(cCons, GRB_LESS_EQUAL, cracks, "nclients_cons");
      model.addConstr(sCons, GRB_LESS_EQUAL , sracks, "nservers_cons");

      model.setObjective(obj, GRB_MAXIMIZE);

      model.write("debug.lp");

      model.optimize();

      int cCovered, sCovered;
      for(int i=0; i<NSW; i++){
         int valC = nodeC[i].get(GRB_DoubleAttr_X);
         int valS = nodeS[i].get(GRB_DoubleAttr_X);
         //cout<<valC<<" "<<valS<<endl;
         if(valC>0){
            cout<<"Client rack: "<<i<<endl;
            clientracks.push_back(i);
            cCovered += getHostsInRack(i);
         }
         if(valS>0){
            cout<<"Server rack: "<<i<<endl;
            serverracks.push_back(i);
            sCovered += getHostsInRack(i);
         }
      }
      while(cCovered < clients){
         //cout<<cCovered<<" "<<clients<<" "<<clientracks<<endl;
         int rack;
         do{
            rack = rand()%NSW;
         }
         while(find(clientracks.begin(), clientracks.end(), rack) != clientracks.end() || find(serverracks.begin(), serverracks.end(), rack) != serverracks.end());
         cCovered = min(clients, cCovered + getHostsInRack(rack));
      }
      while(sCovered < servers){
         int rack;
         do{
            rack = rand()%NSW;
         }
         while(find(clientracks.begin(), clientracks.end(), rack) != clientracks.end() || find(serverracks.begin(), serverracks.end(), rack) != serverracks.end());
         sCovered = min(servers, sCovered + getHostsInRack(rack));
      }
   } catch(GRBException e) {
      cout << "Error code = " << e.getErrorCode() << endl;
      cout << e.getMessage() << endl;
   } catch(...) {
      cout << "Exception during optimization" << endl;
   }

   return pair<vector<int>, vector<int> > (clientracks, serverracks);
}
*/


int rrg_randpath_weighted(vector<route_t*>* paths){
   int npaths = paths->size();

   map<int, int> pathcnt;

   int shortest = 1000000; //INFINITY
   for(int i=0; i<paths->size(); i++){
	shortest = min(shortest, (int)(paths->at(i))->size());
   }
   for(int i=0; i<paths->size(); i++){
	int slack = paths->at(i)->size() - shortest;
	pathcnt[slack]++;
   }
   map<int, double> slackwt;
   slackwt[0] = 1;
   slackwt[2] = 0.4;
   slackwt[4] = 0.2;
   slackwt[6] = 0.1;
   
   double amp = 0;
   //cout<<"Npaths: "<<paths->size()<<" Shortest: "<<shortest<<", slacks: [";
   for(int i=0; i<paths->size(); i++){
	int slack = paths->at(i)->size() - shortest;
	//double wt = slackwt[slack]/pathcnt[slack];
	//double wt = 1.0;	
	//if(slack/2 <= 1) wt = 0.5;
	//else if(slack/2 <= 2) wt = 0.333;
	//else if(slack/2 <= 3) wt = 0.25;
	//else wt = 0;
	double wt = 1.0/(((double)slack/LAMBDA)+1);
	amp += wt;
	//cout<<"("<<slack<<","<<wt<<") ";
   }
   double prob = drand48(), curr_amp=-0.00000000001; //epsilon
   int ret=0;
   //cout<<"] Prob: "<<prob;
   while(curr_amp < prob * amp && ret < paths->size()){
      int slack = paths->at(ret)->size() - shortest;
      //double wt = slackwt[slack]/pathcnt[slack];
      //double wt = 1;	
      //if(slack/2 <= 1) wt = 0.5;
      //else if(slack/2 <= 2) wt = 0.333;
      //else if(slack/2 <= 3) wt = 0.25;
      //else wt = 0;
      double wt = 1.0/(((double)slack/LAMBDA)+1);
      curr_amp += wt;
      ret++;
      //cout<<" [curr_amp:"<<curr_amp<<",ret:"<<ret<<",wt:"<<wt;
   }
   //cout<<" ret: "<<ret-1<<endl;
   return ret-1;
}


void RandRegularTopology::populateNextHops(int dest_sw){

        assert(find_path_alg == ECMP || find_path_alg == ECMP_DAG);
	  	nextHops[dest_sw] = map<int, map<BaseVertex*, double> >();
	  	queue<BaseVertex*> bfsQueue;
		bfsQueue.push(myGraph->get_vertex(dest_sw));
	  	while(!bfsQueue.empty()){
	  		BaseVertex* node = bfsQueue.front();
			bfsQueue.pop();
			set<BaseVertex*> possible_next_hops;
			myGraph->get_adjacent_vertices(node, possible_next_hops);
			for(BaseVertex* next_hop: possible_next_hops){
				if(shortestPathLen[next_hop->getID()][dest_sw] == 1 + shortestPathLen[node->getID()][dest_sw]){
					if(nextHops[dest_sw].find(next_hop->getID()) == nextHops[dest_sw].end())
						bfsQueue.push(next_hop);
					nextHops[dest_sw][next_hop->getID()][node] = 1.0;
				}
			}
	  	}

        /* ECMP hops populated, now add some extra edges for ECMP DAG*/
        if(find_path_alg == ECMP_DAG){
		    for(int k=2; k<=3; k++){
			    set<int> nodesAtDistK;
			    vector<BaseVertex*> allVertices(myGraph->get_allVertices().begin(), myGraph->get_allVertices().end());
			    std::random_shuffle ( allVertices.begin(), allVertices.end() );  
			    for (BaseVertex* v: allVertices){
				    if(shortestPathLen[dest_sw][v->getID()] == k){
					    nodesAtDistK.insert(v->getID());
			    	}
			    }
			    int numExtraEdges = 0;
			    while(!nodesAtDistK.empty()){
				    int node = *(nodesAtDistK.begin());
				    nodesAtDistK.erase(node);
				    set<BaseVertex*> possible_next_hops;
				    myGraph->get_adjacent_vertices(myGraph->get_vertex(node), possible_next_hops);
				    for(BaseVertex* next_hop: possible_next_hops){
					    if(nodesAtDistK.find(next_hop->getID()) != nodesAtDistK.end()){
						    nodesAtDistK.erase(next_hop->getID());
						    nextHops[dest_sw][next_hop->getID()][myGraph->get_vertex(node)] = 0.5; //1.0/k;
						    nextHops[dest_sw][node][next_hop] = 0.5; //1.0/k;
						    numExtraEdges++;
						    break;
					    }
				    }
			    }
			    cout<<"At dist: "<<k<<", Found "<<numExtraEdges<<" extra edges for dest: "<<dest_sw<<endl;
		    }
        }
}



/*
int RandRegularTopology::find_destination(Queue* queue){
  //first check nlp_ns
  for (int i=0;i<NK;i++)
    for (int j = 0;j<NSRV;j++)
      if (queues_nlp_ns[i][j]==queue)
	return j;

  return -1;
}

void RandRegularTopology::print_path(std::ofstream &paths,int src,route_t* route){
  paths << "SRC_" << src << " ";
  
  if (route->size()/2==2){
    paths << "LS_" << find_lp_switch((RandomQueue*)route->at(1)) << " ";
    paths << "DST_" << find_destination((RandomQueue*)route->at(3)) << " ";
  } else if (route->size()/2==4){
    paths << "LS_" << find_lp_switch((RandomQueue*)route->at(1)) << " ";
    paths << "US_" << find_up_switch((RandomQueue*)route->at(3)) << " ";
    paths << "LS_" << find_lp_switch((RandomQueue*)route->at(5)) << " ";
    paths << "DST_" << find_destination((RandomQueue*)route->at(7)) << " ";
  } else if (route->size()/2==6){
    paths << "LS_" << find_lp_switch((RandomQueue*)route->at(1)) << " ";
    paths << "US_" << find_up_switch((RandomQueue*)route->at(3)) << " ";
    paths << "CS_" << find_core_switch((RandomQueue*)route->at(5)) << " ";
    paths << "US_" << find_up_switch((RandomQueue*)route->at(7)) << " ";
    paths << "LS_" << find_lp_switch((RandomQueue*)route->at(9)) << " ";
    paths << "DST_" << find_destination((RandomQueue*)route->at(11)) << " ";
  } else {
    paths << "Wrong hop count " << ntoa(route->size()/2);
  }
  
  paths << endl;
}
*/
