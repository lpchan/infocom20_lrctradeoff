/*
 * For better understanding of the code, especially how the coordinator 
 * generates the decode/upcode/downcode commands, pls read the code while 
 * referring to [[[Fig.4 in our infocom20 paper]]]. 
 * We explain our code progressively using [[[Fig.4]]] as examples.
 */

#include "Coordinator.hh"

Coordinator::Coordinator(Metadata* newMeta, Config* config, Socket* cn2dnSocket, Socket* dn2dnSocket){
  meta = newMeta;
  conf = config;
  cn2dnSoc = cn2dnSocket;
  dn2dnSoc = dn2dnSocket;
  k = conf->k;
  l_f = conf->l_f;
  g = conf->g;
  l_c = conf->l_c;
  chunk_size = 1024*1024*conf->chunk_size;
  packet_size = 1024*1024*conf->packet_size;
  place_method = conf->place_method;
}

Coordinator::~Coordinator(){
}

  // send command 
void Coordinator::sendCmd(string cmd, string dest_IP){
  cn2dnSoc->sendData((char*)cmd.c_str(), cmd.length(), cmd.length(), (char*)dest_IP.c_str(), DN_RECV_CMD_PORT);
}

  // receive ack
int Coordinator::recvAck(char* ack){
  int BUFSIZE = 1024;
  int ack_length = cn2dnSoc->recvCmd(CN_RECV_ACK_PORT, BUFSIZE, ack);
  ack[ack_length] = '\0';
  cout<<"****** recieve ack: "<<ack<<endl;
  return ack_length;
}

  // test the performance of upload, download, upcode and downcode
void Coordinator::testPerformance(string file) {
  uploadFile(file);
  FILE* fpr = fopen("./results", "a");
  fprintf(fpr, "------ k: %d, l_f: %d, g: %d, l_c: %d\n ", k, l_f, g, l_c);
  double temp_decode_time_f = 0.0;
  double temp_upcode_time = 0.0;
  double temp_decode_time_c = 0.0;
  double temp_downcode_time = 0.0;
  int running_times = 1;
  for(int i = 0; i < running_times; ++i) {
    double temp_de1, temp_up, temp_de2, temp_do;
    temp_de1 = temp_up = temp_de2 = temp_do = 0.0;
    for(int idx = 0; idx < k; ++idx) {
      temp_de1 += downloadFile(file, idx);
    }
    temp_decode_time_f += temp_de1;
    temp_up = upcodeFile(file);
    temp_upcode_time += temp_up;
    for(int idx = 0; idx < k; ++idx) {
      temp_de2 += downloadFile(file, idx);
    }
    temp_decode_time_c += temp_de2;
    temp_do = downcodeFile(file);
    temp_downcode_time += temp_do;
    temp_de1 /= k;
    temp_de2 /= k;
    fprintf(fpr, "%d th round\n", i);
    fprintf(fpr, "~~~~~~ fast side, decode time: %.21f s\n ", temp_de1);
    fprintf(fpr, "###### upcode time: %.21f s\n ", temp_up);
    fprintf(fpr, "~~~~~~ compact side, decode time: %.21f s\n ", temp_de2);
    fprintf(fpr, "%%%%%% downcode time: %.21f s\n ", temp_do);
  }

  temp_decode_time_f /= k;
  temp_decode_time_f /= running_times;
  temp_upcode_time /= running_times;
  temp_decode_time_c /= k;
  temp_decode_time_c /= running_times;
  temp_downcode_time /= running_times;

  fprintf(fpr, "\nin toal\n");
  fprintf(fpr, "~~~~~~ fast side, decode time: %.21f s\n ", temp_decode_time_f);
  fprintf(fpr, "###### upcode time: %.21f s\n ", temp_upcode_time);
  fprintf(fpr, "~~~~~~ compact side, decode time: %.21f s\n ", temp_decode_time_c);
  fprintf(fpr, "%%%%%% downcode time: %.21f s\n ", temp_downcode_time);
  fclose(fpr);
}

  // send a block when uploading, wait and receive ack
int Coordinator::CNSendData(int blk_id, string blk_name, char* buf, string blk_ip, char* ack) {
  string cmd = "en";
  cmd += blk_name;
  cout<<"~~~upload block "<<blk_id<<", send cmd: "<<cmd<<endl;
  sendCmd(cmd, blk_ip);
  cout<<"~~~then send data!"<<endl;
  cn2dnSoc->sendData(buf, chunk_size, packet_size, (char*)blk_ip.c_str(), CN_UP_DATA_PORT);
  int ack_len = recvAck(ack);
  cout<<"ack length: "<<ack_len<<endl;
  return ack_len;
}

  // calculate local parity block when uploading
void Coordinator::calculateLocalParityBlock(int local_blk_id, char** buf) {
  int* int_buf = (int*)buf[local_blk_id];
  int lp_id = local_blk_id - k;
  int* dat_buf;
  int r_f = k / l_f;
  for(int dat_blk_id = lp_id*r_f; dat_blk_id < lp_id*r_f + r_f; ++dat_blk_id) {
    dat_buf = (int*)buf[dat_blk_id];
    for(int num = 0; num < (long long)(chunk_size * sizeof(char) / sizeof(int)); ++num) {
      int_buf[num] = int_buf[num] ^ dat_buf[num];
    }
  }
}

  /* * * * * * * * * * * * * * * * * * * *
   *    kernel routine 1: uploadFile     *
   * * * * * * * * * * * * * * * * * * * */
void Coordinator::uploadFile(string file){
  // [1st], file info
  int file_size = 0;
  FILE* fp = fopen((char*)file.c_str(), "r");
  if(fp != NULL) {
    fseek(fp, 0L, SEEK_END);
    file_size = ftell(fp);
    fclose(fp);
  }
  int file_num = meta->getFileNum();
  cout<<"currently file number: "<<file_num;
  cout<<", to upload a new file ["<<file<<"] with size "<<file_size<<endl;
    // !!! update file metadata
  meta->setFileNum(file_num + 1);
  meta->updateFileNames(file);
  meta->updateFileSizes(file, file_size);

    // variables for stripes
  set<string> stripes;
  stripes.clear();
  int temp_stripe_num = file_size / (k * chunk_size);
    // we assume special-format files, such as "FI0000", "FI0001", whose names are composed of six chars.
	// we then add four numbers to represent a stripe, such as "0000", "0001".
	// we next add two numbers to represent a block, such as "00", "01".
	// e.g., the first block in the first stripe of file "FI0000" is represented by "FI0000-0000-00".
  int stripe_name_len = 6 + 1 + 4 + 1;
  int blk_name_len = 6 + 1 + 4 + 1 + 2 + 1;
  char* stripe_name = new char[stripe_name_len];
  char* blk_name = new char[blk_name_len];
  strcpy(stripe_name, (char*)file.c_str());
  strcpy(blk_name, (char*)file.c_str());
  stripe_name[6] = '-';
  blk_name[6] = '-';
  stripe_name[7] = '\0';
  blk_name[7] = '\0';
    // variables for blocks
  set<pair<unsigned int, string>> blocks;
  blocks.clear();
  int thousand;
  int handred;
  int ten;
  int one;

    // we new a stripe-length (i.e., k + l_f) of bufs 
	// to store the data and local parity blocks.
  char** buf = new char*[k + l_f];
  for(int i = 0; i < k + l_f; ++i) {
    buf[i] = new char[chunk_size];
  }
    // open file
  FILE* fp2 = fopen((char*)file.c_str(), "r");
  if(fp2 == NULL) {
    cout<<"WARNING: file not exist, cannot proceed..."<<endl;
    return;
  }
    // when we send the buf of blocks to k + l_f different storage nodes, 
	// the coordinator will wait and receive an ack from each of the k + l_f nodes.
  int ack_size = 1024;
  char** acks = new char*[k + l_f];
  for(int i = 0; i < k + l_f; ++i) {
    acks[i] = new char[ack_size];
  }
  int* ack_lens = new int[k + l_f];

  // [2nd], stripe info
  int all_stripe_succ_tag = 1;
  for(int i = 0; i < temp_stripe_num; ++i) {
    thousand = i/1000;
    handred = (i - thousand * 1000)/100;
    ten = (i - thousand * 1000 - handred * 100)/10;
    one = i - thousand * 1000 - handred * 100 - ten * 10;
    stripe_name[7] = blk_name[7] = '0' + thousand;
    stripe_name[8] = blk_name[8] = '0' + handred;
    stripe_name[9] = blk_name[9] = '0' + ten;
    stripe_name[10] = blk_name[10] = '0' + one;
    stripe_name[11] = '\0';
    blk_name[11] = '-';
    blk_name[12] = '\0';
    stripes.insert(string(stripe_name));
      // !!! update file metadata
    meta->updateStripeFiles(string(stripe_name), file);

    int stripe_num = meta->getStripeNum();
      // !!! update stripe metadata
    meta->setStripeNum(stripe_num + 1);
    meta->updateStripeNames(string(stripe_name));

      // we select locations for the n buf of blocks.
    map<int, string> blk_id2IP = decide_location();
    map<int, string>::const_iterator blk_id2IPIter;
    int temp_blk_id;
    string temp_blk_IP;

    // [3rd], block info
    blocks.clear();
    int all_block_succ_tag = 1;
    for(int blk_id = 0; blk_id < k + l_f; ++blk_id) {
      ten = blk_id / 10;
      one = blk_id - ten * 10;
      blk_name[12] = '0' + ten;
      blk_name[13] = '0' + one;
      blk_name[14] = '\0';
      blocks.insert(pair<unsigned int, string>(blk_id, string(blk_name)));
        // !!! update stripe metadata
      meta->updateBlkStripes(string(blk_name), string(stripe_name));
      
	    // fill each block
      if(blk_id < k) {
          // data blocks
        fread(buf[blk_id], 1, chunk_size, fp2);
      } else {
          // local parity blocks
        calculateLocalParityBlock(blk_id, buf);
      }
        // send each block
      for(blk_id2IPIter = blk_id2IP.begin(); blk_id2IPIter != blk_id2IP.end(); ++blk_id2IPIter) {
        temp_blk_id = (*blk_id2IPIter).first;
        temp_blk_IP = (*blk_id2IPIter).second;
        if(temp_blk_id == blk_id && blk_id < k + l_f) {
          ack_lens[blk_id] = CNSendData(temp_blk_id, string(blk_name), buf[blk_id], temp_blk_IP, acks[blk_id]);
          if(strcmp(acks[blk_id], "write blk success") != 0) {
            cout<<"write block "<<blk_id<<" fail!"<<endl;
            all_block_succ_tag = -1;
          }
            // !!! update stripe metadata
          meta->updateBlkIPs((string)blk_name, temp_blk_IP);
          break;
        }
      }
    } // end of block info
    if(all_block_succ_tag == -1) {
      all_stripe_succ_tag = -1;
    }
      // !!! update stripe metadata
    meta->updateStripeBlks(string(stripe_name), blocks);

  } // end of stripe info

  if(all_stripe_succ_tag == 1) {
    cout<<"****** Finish uplode !"<<endl;
  } else {
    cout<<"****** Upload fail !"<<endl;
  }
    // !!! update file metadata
  meta->updateFileStripes(file, stripes);
  meta->updateFileHots(file);
  
  delete stripe_name;
  delete blk_name;
  for(int i = 0; i < k + l_f; ++i) {
    delete buf[i];
  }
  delete buf;
  if(fp2 != NULL) {
    fclose(fp2);
  }
  for(int i = 0; i < k + l_f; ++i){
    delete acks[i];
  }
  delete acks;
  delete ack_lens;
}

  // decide the stripe's blocks' locations
  // main function for cluster-aware placement
map<int, string> Coordinator::decide_location() {
  map<int, string> blk_id2IP;

  set<string> racks = meta->getRacks();
  int rack_num = racks.size();
  string temp_racks[rack_num];
  int* dn_num_each_rack = new int[rack_num];
  set<string> dns;
  dns.clear();
  set<string>::const_iterator racksIter;
  int idx = 0;
  for(racksIter = racks.begin(); racksIter != racks.end(); ++racksIter) {
    temp_racks[idx] = *racksIter;
    dns = meta->getRack2DN(temp_racks[idx]);
    dn_num_each_rack[idx] = dns.size();
    idx++;
  }

  int delta = l_f / l_c;
  int r_f = k / l_f;
  int r_c = k / l_c;

  // sort the racks according to the number of dns in each rack
  for(int i = 0; i < rack_num - 1; ++i) {
    for(int j = rack_num - 1; j > i; --j) {
      if(dn_num_each_rack[j] > dn_num_each_rack[j-1]) {
        int temp = dn_num_each_rack[j];
        dn_num_each_rack[j] = dn_num_each_rack[j-1];
        dn_num_each_rack[j-1] = temp;
        string str = temp_racks[j];
        temp_racks[j] = temp_racks[j - 1];
        temp_racks[j - 1] = str;
      }
    }
  }

  if(place_method == OPT_S) {
    for(int i = 0; i < l_c; ++i) {
      string core_rack = temp_racks[i];
      dns = meta->getRack2DN(core_rack);
      int dn_num = dns.size();
      string temp_dns[dn_num];
      int index = 0;
      set<string>::const_iterator dnsIter;
      for(dnsIter = dns.begin(); dnsIter != dns.end(); ++dnsIter) {
        temp_dns[index++] = *dnsIter;
      }
      index = 0;
      for(int blk_id = i*r_c; blk_id < i*r_c + r_f; ++blk_id) {
        if(dn_num == 1) {
          blk_id2IP.insert(pair<int, string>(blk_id, temp_dns[0]));
        } else {
          blk_id2IP.insert(pair<int, string>(blk_id, temp_dns[index++]));
        }
      }
      for(int lp = i*delta; lp < i*delta + delta; ++lp) {
        int temp_lp = lp + k;
        if(dn_num == 1) {
          blk_id2IP.insert(pair<int, string>(temp_lp, temp_dns[0]));
        } else {
          blk_id2IP.insert(pair<int, string>(temp_lp, temp_dns[index++]));
        }
      }
      for(int j = 0; j < delta - 1; ++j) {
        string a_different_rack = temp_racks[l_c + i*(delta-1) + j];
        dns = meta->getRack2DN(a_different_rack);
        dn_num = dns.size();
        string temptemp_dns[dn_num];
        index = 0;
        for(dnsIter = dns.begin(); dnsIter != dns.end(); ++dnsIter) {
          temptemp_dns[index++] = *dnsIter;
        }
        index = 0;
        for(int blk_id = i*r_c + (j+1)*r_f; blk_id < i*r_c + (j+2)*r_f; ++blk_id) {
          if(dn_num == 1) {
            blk_id2IP.insert(pair<int, string>(blk_id, temptemp_dns[0]));
          } else {
            blk_id2IP.insert(pair<int, string>(blk_id, temptemp_dns[index++]));
          }
        }
      }
    } // end of outter for

    /*
    string gp_rack = temp_racks[l_c * delta];
    dns = meta->getRack2DN(gp_rack);
    int dn_num = dns.size();
    string temp_dns[dn_num];
    int index = 0;
    set<string>::const_iterator dnsIter;
    for(dnsIter = dns.begin(); dnsIter != dns.end(); ++dnsIter) {
      temp_dns[index++] = *dnsIter;
    }
    index = 0;
    for(int gpIdx = 0; gpIdx < g; ++gpIdx) {
      int temp_gp = gpIdx + k + l_f;
      if(dn_num == 1) {
        blk_id2IP.insert(pair<int, string>(temp_gp, temp_dns[0]));
      } else {
        blk_id2IP.insert(pair<int, string>(temp_gp, temp_dns[index++]));
      }
    }
    */ // Note: this part of code may be useful in future for placing global parity blocks

  } // end of if place_method == OPT_S

  if(place_method == OPT_R) {
    for(int i = 0; i < l_c * delta; ++i) {
      string rack = temp_racks[i];
      dns = meta->getRack2DN(rack);
      int dn_num = dns.size();
      string temp_dns[dn_num];
      set<string>::const_iterator dnsIter;
      int index = 0;
      for(dnsIter = dns.begin(); dnsIter != dns.end(); ++dnsIter) {
        temp_dns[index++] = *dnsIter;
      }
      index = 0;
      if(i < l_c * delta) {
        for(int blk_id = i*r_f; blk_id < i*r_f + r_f; ++blk_id) {
          if(dn_num == 1) {
            blk_id2IP.insert(pair<int, string>(blk_id, temp_dns[0]));
          } else {
            blk_id2IP.insert(pair<int, string>(blk_id, temp_dns[index++]));
          }
        }
        int temp_lp = k + i;
        if(dn_num == 1) {
          blk_id2IP.insert(pair<int, string>(temp_lp, temp_dns[0]));
        } else {
          blk_id2IP.insert(pair<int, string>(temp_lp, temp_dns[index++]));
        }
      }
      /*
      else {
        for(int gpIdx = 0; gpIdx < g; ++gpIdx) {
          int temp_gp = gpIdx + k + l_f;
          if(dn_num == 1) {
            blk_id2IP.insert(pair<int, string>(temp_gp, temp_dns[0]));
          } else {
            blk_id2IP.insert(pair<int, string>(temp_gp, temp_dns[index++]));          }
        }
      }
      */ // Note: this part of code may be useful in future for placing global parity blocks
    }
  } // end of if place_method == OPT_R

  if(place_method == FLAT) {
    for(int i = 0; i < k + l_f; ++i) {
      string rack = temp_racks[i];
      dns = meta->getRack2DN(rack);
      int dn_num = dns.size();
      string temp_dns[dn_num];
      set<string>::const_iterator dnsIter;
      int index = 0;
      for(dnsIter = dns.begin(); dnsIter != dns.end(); ++dnsIter) {
        temp_dns[index++] = *dnsIter;
      }
      index = 0;
      blk_id2IP.insert(pair<int, string>(i, temp_dns[0]));
    }
  } // end of if place_method == FLAT

  cout<<"block placement:"<<endl;
  map<int, string>::const_iterator blk_id2IPIter;
  for(blk_id2IPIter = blk_id2IP.begin(); blk_id2IPIter != blk_id2IP.end(); ++blk_id2IPIter) {
    if((*blk_id2IPIter).first < k) {
      cout<<"data block "<<(*blk_id2IPIter).first<<" ---> "<<(*blk_id2IPIter).second<<endl;
    } else if ((*blk_id2IPIter).first < k + l_f) {
      cout<<"local parity block "<<(*blk_id2IPIter).first - k<<" ---> "<<(*blk_id2IPIter).second<<endl;
    } else {
      cout<<"global parity block "<<(*blk_id2IPIter).first - k - l_f<<" ---> "<<(*blk_id2IPIter).second<<endl;
    }
  }

  delete dn_num_each_rack;
  return blk_id2IP;
}

  /* * * * * * * * * * * * * * * * * * * *
   *   kernel routine 2: downloadFile    *
   * * * * * * * * * * * * * * * * * * * */
double Coordinator::downloadFile(string file, int sim_miss_id){
  double decode_time = 0.0;
  set<string> stripes = meta->getFile2Stripes(file);
  string tmp_stripe;
  set<pair<unsigned int, string>> tmpBlocks;
  unsigned int tmp_block_idx;
  string tmp_block;
  string tmp_IP;
  int stripe_len;
  bool hot_tag = meta->isFileHot(file);
  if(hot_tag){
    stripe_len = k + l_f; //k + l_f + g;
  } else {
    stripe_len = k + l_c; //k + l_c + g;
  }
  string temp_blocks[stripe_len];
  string temp_IPs[stripe_len];
  int ack_size = 1024;
  char** acks = new char*[k];
  for(int i = 0; i < k; ++i) {
    acks[i] = new char[ack_size];
  }
  int* ack_lens = new int[k];

  string gw_ip = meta->getGW();

  FILE* fp = fopen("./output", "w");
  if(fp == NULL) {
    cout<<"open file error!"<<endl;
    return decode_time;
  }
  int BUFSIZE = chunk_size*k;
  char* buf = new char[BUFSIZE];
  int packet_num = chunk_size / packet_size;
  int* mark_recv = new int[packet_num*k];
  char** source_IPs_recv_data = new char*[k];
  for(int i = 0; i < k; ++i){
    source_IPs_recv_data[i] = new char[20];
  }
  for(int j = 0; j < packet_num*k; ++j) {
    mark_recv[j] = -1;
  }

  set<string>::const_iterator stripesIter;
  for(stripesIter = stripes.begin(); stripesIter != stripes.end(); ++stripesIter){
    tmp_stripe = *stripesIter;
    tmpBlocks = meta->getStripe2Blocks(tmp_stripe);
    set<pair<unsigned int, string>>::const_iterator tmpBlocksIter;
    // 1st, request blocks
    cout<<"****** CN analyzies a new stripe ******"<<endl;
    for(tmpBlocksIter = tmpBlocks.begin(); tmpBlocksIter != tmpBlocks.end(); ++tmpBlocksIter){
      tmp_block_idx = (*tmpBlocksIter).first;
      tmp_block = (*tmpBlocksIter).second;
      tmp_IP = meta->getBlock2IP(tmp_block);
      temp_blocks[tmp_block_idx] = tmp_block;
      temp_IPs[tmp_block_idx] = tmp_IP;
      cout<<"block "<<tmp_block_idx<<", "<<tmp_block<<", IP: "<<tmp_IP<<endl;
      if(tmp_block_idx < (unsigned int)k) {
        cout<<"###request block "<<tmp_block_idx<<" , send cmd###"<<endl;
      }
      if(tmp_block_idx < (unsigned int)k){
        string cmd = "dl" + tmp_block;
        sendCmd(cmd, tmp_IP);
        ack_lens[tmp_block_idx] = recvAck(acks[tmp_block_idx]);
        cout<<"ack length: "<<ack_lens[tmp_block_idx]<<endl;
      }
    }

    // 2rd, receive blocks
    bool block_miss = false;
    int missing_ID = -1;
    for(int i = 0; i < k; ++i) {
      if(strcmp(acks[i], "blk_ex") == 0){
        // do nothing
      } else if(strcmp(acks[i], "blk_mi") == 0) {
        block_miss = true;
        missing_ID = i;
        break;
      }
    }
    if(!block_miss) {
      cout<<"###### all block exist ###### "<<endl;
      // simulate block miss
      cout<<"###### simulate block miss ###### "<<endl;
      missing_ID = sim_miss_id;
    }
 
    cout<<"~~~~~~ data block "<<missing_ID<<" fails ~~~~~~"<<endl;
    cout<<"trigger decode..."<<endl;
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);
    int startDataIdx = requiredStartDataBlkID(missing_ID, hot_tag);
    int endDataIdx = requiredEndDataBlkID(missing_ID, hot_tag);
    int localParityIdx = requiredLocalParityBlkID(missing_ID, hot_tag);

    char* gw_cmd = new char[400];
    gw_cmd[0] = '\0';

    for(int index = startDataIdx; index <= endDataIdx; ++index){
      string cmd = generateDecodeCmd(temp_blocks, temp_IPs, index, missing_ID, hot_tag, gw_ip, gw_cmd);
      sendCmd(cmd, temp_IPs[index]);
      cout<<"~~~~~~ send cmd to data block "<<index<<" :"<<cmd<<endl;
    }
    string cmd = generateDecodeCmd(temp_blocks, temp_IPs, localParityIdx, missing_ID, hot_tag, gw_ip, gw_cmd);
    sendCmd(cmd, temp_IPs[localParityIdx]);
    cout<<"~~~~~~ send cmd to local parity block "<<(localParityIdx - k)<<" :"<<cmd<<endl;

    if(gw_cmd[0] != '\0') {
      cout<<"gw "<<gw_ip<<", decode cmd: "<<gw_cmd<<endl;
      sendCmd(string(gw_cmd), gw_ip);
    }
    delete gw_cmd;

    int final_ack_size = 1024;
    char* final_ack = new char[final_ack_size];
    int final_ack_len = recvAck(final_ack);
    cout<<"final ack length: "<<final_ack_len<<endl;
    if(strcmp(final_ack, "fi_deco") == 0) {
      // TODO, to ready download again
      cout<<"~~~~~~ recieve finish decode !"<<endl;
      gettimeofday(&end_time, NULL);
      decode_time = end_time.tv_sec-start_time.tv_sec+(end_time.tv_usec-start_time.tv_usec)*1.0/1000000;
      fprintf(stderr, "~~~~~~ decode time: %.2lf s\n", decode_time);
    }

    // download file again
    for(int i = 0; i < k; ++i) {
      string re_download_cmd = "re";
      sendCmd(re_download_cmd, temp_IPs[i]);
      cout<<"send ready to download cmd "<<i<<": "<<re_download_cmd<<endl;
    }
    cn2dnSoc->paraRecvData(CN_DO_DATA_PORT, buf, chunk_size, packet_size, k, mark_recv, DATA_CHUNK, source_IPs_recv_data);
    //cout<<"source_IPs_recv_data: "<<endl;
      //for(int i = 0; i < k; ++i){
      //  cout<<source_IPs_recv_data[i]<<endl;
    //}
    int write_len = 0;
    for(int i = 0; i < k; ++i){
      int index = 0;
      for(; index < k; ++index){
        if(strcmp(source_IPs_recv_data[index], (char*)temp_IPs[i].c_str()) == 0){
          break;
        }
      }
      write_len = fwrite(buf + index*chunk_size, 1, chunk_size, fp);
      //cout<<"write size: "<<write_len<<endl;
    }

  }// end of outer for

  delete buf;
  delete mark_recv;
  fclose(fp);
  for(int i = 0; i < k; ++i){
    delete source_IPs_recv_data[i];
  }
  delete source_IPs_recv_data;

  for(int i = 0; i < k; ++i){
    delete acks[i];
  }
  delete acks;
  delete ack_lens;
  tmpBlocks.clear();
  stripes.clear();
  return decode_time;
}

  // functions required for decode
  // decide the local parity block id related to a missing data block
int Coordinator::requiredLocalParityBlkID(int missing_ID, bool hot_tag){
  int r_f = k / l_f;
  int r_c = k / l_c;
  int tmpLocalParityBlkID;
  if(hot_tag) {
    tmpLocalParityBlkID = missing_ID / r_f;
  } else {
    tmpLocalParityBlkID = missing_ID / r_c;
  }
  return (tmpLocalParityBlkID + k);
}

  // decide the start data block id of the local data set related to a missing data block
int Coordinator::requiredStartDataBlkID(int missing_ID, bool hot_tag){
  int r_f = k / l_f;
  int r_c = k / l_c;
  int tmpID;
  if(hot_tag) {
    tmpID = missing_ID / r_f;
    return tmpID * r_f;
  } else {
    tmpID = missing_ID / r_c;
    return tmpID * r_c;
  }
}

  // decide the end data block id of the local data set related to a missing data block
int Coordinator::requiredEndDataBlkID(int missing_ID, bool hot_tag){
  int r_f = k / l_f;
  int r_c = k / l_c;
  int tmpID;
  if(hot_tag) {
    tmpID = missing_ID / r_f;
    return tmpID * r_f + r_f - 1;
  } else {
    tmpID = missing_ID / r_c;
    return tmpID * r_c + r_c - 1;
  }
}

  // test decode command
void Coordinator::testDecodeCmd(int missing_ID){
  string stripe_blks[k + l_f];
  stripe_blks[0] = "data0";
  stripe_blks[1] = "data1";
  stripe_blks[2] = "data2";
  stripe_blks[3] = "data3";
  stripe_blks[4] = "lf0";
  stripe_blks[5] = "lf1";
  string blk_IPs[k + l_f];
  blk_IPs[0] = "192.168.0.22";
  blk_IPs[1] = "192.168.0.23";
  blk_IPs[2] = "192.168.0.26";
  blk_IPs[3] = "192.168.0.27";
  blk_IPs[4] = "192.168.0.24";
  blk_IPs[5] = "192.168.0.25";
  bool hot_or_not = false;
  string gw_ip = meta->getGW();
  char* gw_cmd = new char[100];
  gw_cmd[0] = '\0';
  for(int blk_id = 0; blk_id < k; ++blk_id) {
    cout<<"blk id "<<blk_id<<" , decode cmd: "<<generateDecodeCmd(stripe_blks, blk_IPs, blk_id, missing_ID, hot_or_not, gw_ip, gw_cmd)<<endl;
  }
  int blk_id = 4;
  cout<<"blk id "<<blk_id<<" , decode cmd: "<<generateDecodeCmd(stripe_blks, blk_IPs, blk_id, missing_ID, hot_or_not, gw_ip, gw_cmd)<<endl;
  blk_id = 5;
  cout<<"blk id "<<blk_id<<" , decode cmd: "<<generateDecodeCmd(stripe_blks, blk_IPs, blk_id, missing_ID, hot_or_not, gw_ip, gw_cmd)<<endl;

  if(gw_cmd[0] != '\0') {
    cout<<"gw cmd: "<<gw_cmd<<endl;
  }

  delete gw_cmd;
}

  /* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * 
   *                    generate commands for decode                     *
   *                                                                     *
   * e.g., in Fig.4 of our infocom20 paper, in the compact LRC,          *
   * when D0 fails, D0 = L0' + D1 + D2 + D3 + D4 + D5,                   *
   *                                                                     *
   * 1) L0' and D1 will send blocks directly to D0 as they reside in     * 
   * the same rack/ cluster,                                             *
   *                                                                     *
   * 2) D3 will send its content to D2. D2 will calculate an XOR sum of  *
   * D2+D3, and then send D2+D3 to the gateway, which will re-send D2+D3 *
   * to D0 (cross-cluster),                                              *
   *                                                                     *
   * 3) D5 will send its content to D4. D4 will calculate an XOR sum of  *
   * D4+D5, and then send D4+D5 to the gateway, which will re-send D4+D5 *
   * to D0 (cross-cluster),                                              *
   *                                                                     *
   * 4) D0 recomputes itself based on L0', D1, D2+D3, D4+D5              *
   * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
string Coordinator::generateDecodeCmd(string stripe_blks[], string blk_IPs[], int blk_id, int missing_ID, bool hot, string gw_ip, char* gw_cmd){
  string block = stripe_blks[blk_id];
  string block_ip = blk_IPs[blk_id];
  string rack = meta->getDN2Rack(block_ip);
  string missing_block = stripe_blks[missing_ID];
  string missing_block_ip = blk_IPs[missing_ID];
  string missing_block_rack = meta->getDN2Rack(missing_block_ip);
  string retCmd = "";  
  int startIdx = requiredStartDataBlkID(missing_ID, hot);
  int endIdx = requiredEndDataBlkID(missing_ID, hot);
  int localParityIdx = requiredLocalParityBlkID(missing_ID, hot);

  if(!((startIdx <= blk_id && blk_id <= endIdx) || blk_id == localParityIdx)) {
    return retCmd;
  }

  retCmd += "de";

  // Note: currently, we recover a block in its original place
  if(blk_id == missing_ID) {
    int num_blk_missing_rack = 0;
    int num_wait_other_racks = 0;
    set<int> wait_blk_idx;
    wait_blk_idx.clear();
    string tmp_blk;
    string tmp_ip;
    string tmp_rack;

    set<string> waited_racks;
    waited_racks.clear();
    string waited_gw_ip_concated_str = "";
    string gw_waited_block_ip_concated_str = "";
    // iterate over the data blocks
    for(int idx = startIdx; idx <= localParityIdx; ++idx) {
      if((startIdx <= idx && idx <= endIdx) || idx == localParityIdx) {
        tmp_blk = stripe_blks[idx];
        tmp_ip = blk_IPs[idx];
        tmp_rack = meta->getDN2Rack(tmp_ip);
        if(tmp_rack == missing_block_rack) {
          num_blk_missing_rack++;
          wait_blk_idx.insert(idx);
        } else {
          string other_rack = tmp_rack;
          int smallest_idx_this_rack = idx;
          string tmptmp_rack;
          for(int j = startIdx; j <= endIdx; ++j) {
            tmptmp_rack = meta->getDN2Rack(blk_IPs[j]);
            if(tmptmp_rack == other_rack && j < smallest_idx_this_rack) {
              smallest_idx_this_rack = j;
              break;
            }
          }
          set<string>::const_iterator waited_racks_iter = waited_racks.find(other_rack);
          if(waited_racks_iter == waited_racks.end()) {
            num_wait_other_racks++;
            waited_racks.insert(other_rack);
            waited_gw_ip_concated_str += gw_ip;
            gw_waited_block_ip_concated_str += blk_IPs[smallest_idx_this_rack];
          }
        }
      }
    }

    // missing block command, e.g., [D0 in Fig.4 in paper]
    retCmd += "wa";
    retCmd += to_string(num_blk_missing_rack - 1 + num_wait_other_racks);
    retCmd += "blk";
    set<int>::const_iterator wait_blk_idx_iter;
    for(wait_blk_idx_iter = wait_blk_idx.begin(); wait_blk_idx_iter != wait_blk_idx.end(); ++wait_blk_idx_iter) {
      if((*wait_blk_idx_iter) != blk_id) {
        tmp_ip = blk_IPs[*wait_blk_idx_iter];
        retCmd += tmp_ip;
      }
    }
    retCmd += waited_gw_ip_concated_str;
    retCmd += "reco";

    if(num_wait_other_racks != 0) {
      // gateway command
      string gw_cmd_str = "ga";
      gw_cmd_str += to_string(1);
      gw_cmd_str += "wa";
      gw_cmd_str += to_string(num_wait_other_racks);
      gw_cmd_str += gw_waited_block_ip_concated_str;
      gw_cmd_str += "se";
      gw_cmd_str += missing_block_ip;
      strcpy(gw_cmd, (char*)gw_cmd_str.c_str());
    }

  } else {
    if(rack == missing_block_rack) {
      // command for a block residing in the missing block's rack, but not 
      // the missing block, e.g., [D1 in Fig.4 in paper]
      // "se block missing_block_ip"
      retCmd += "se";
      retCmd += block;
      retCmd += missing_block_ip;
    } else {
      int num_blk_this_rack = 0;
      int smallest_idx_this_rack = blk_id;
      set<int> idx_this_rack;
      idx_this_rack.clear();
      string tmp_blk;
      string tmp_ip;
      string tmp_rack;
      // iterate over the data blocks
      for(int idx = startIdx; idx <= endIdx; ++idx) {
        tmp_blk = stripe_blks[idx];
        tmp_ip = blk_IPs[idx];
        tmp_rack = meta->getDN2Rack(tmp_ip);
        if(tmp_rack == rack) {
          num_blk_this_rack++;
          if(idx < smallest_idx_this_rack) {
            smallest_idx_this_rack = idx;
          }
          idx_this_rack.insert(idx);
        }
      }
      // consider the local parity block
      tmp_blk = stripe_blks[localParityIdx];
      tmp_ip = blk_IPs[localParityIdx];
      tmp_rack = meta->getDN2Rack(tmp_ip);
      if(tmp_rack == rack) {
        num_blk_this_rack++;
        idx_this_rack.insert(localParityIdx);
      }

      if(num_blk_this_rack == 1) {
        retCmd += "se";
        retCmd += block;
        retCmd += gw_ip;
      } else {
        if(smallest_idx_this_rack < blk_id) {
          // e.g., [D3/D5 in Fig.4 in paper]
          tmp_ip = blk_IPs[smallest_idx_this_rack];
          retCmd += "se";
          retCmd += block;
          retCmd += tmp_ip;
        } else {
          // e.g., [D2/D4 in Fig.4 in paper]
          retCmd += "wa";
          retCmd += to_string(num_blk_this_rack - 1);
          retCmd += "blk";
          set<int>::const_iterator idx_this_rack_iter;
          for(idx_this_rack_iter = idx_this_rack.begin(); idx_this_rack_iter != idx_this_rack.end(); ++idx_this_rack_iter) {
            if((*idx_this_rack_iter) != blk_id) {
              tmp_ip = blk_IPs[*idx_this_rack_iter];
              retCmd += tmp_ip;
            }
          }
          retCmd += "se";
          retCmd += block;
          retCmd += gw_ip;
        }
      }
    }
  }
  return retCmd;
}

  /* * * * * * * * * * * * * * * * * * * *
   *    kernel routine 3: upcodeFile     *
   * * * * * * * * * * * * * * * * * * * */
double Coordinator::upcodeFile(string file){
  double upcode_time = 0.0;
  set<string> stripes = meta->getFile2Stripes(file);
  string tmp_stripe;
  set<pair<unsigned int, string>> tmpBlocks;
  unsigned int tmp_block_idx;
  string tmp_block;
  string tmp_IP;
  bool hot_tag = meta->isFileHot(file);
  if(!hot_tag) {
    cout<<"file [ "<<file<<" ] is cold, cannot upcode"<<endl;
    return upcode_time;
  }
  int stripe_len = k + l_f + g;
  string temp_blocks[stripe_len];
  string temp_IPs[stripe_len];

  int ack_size = 1024;
  char** acks = new char*[l_c];
  for(int i = 0; i < l_c; ++i) {
    acks[i] = new char[ack_size];
  }
  int* ack_lens = new int[l_c];

  int stripe_num = stripes.size();
  bool* all_stripe_finish_tag = new bool[stripe_num];
  for(int i = 0; i < stripe_num; ++i) {
    all_stripe_finish_tag[i] = false;
  }
  int stripe_index = 0;

  string gw_ip = meta->getGW();

  struct timeval start_time, end_time;
  set<string>::const_iterator stripesIter;
  for(stripesIter = stripes.begin(); stripesIter != stripes.end(); ++stripesIter){
    tmp_stripe = *stripesIter;
    tmpBlocks = meta->getStripe2Blocks(tmp_stripe);
    set<pair<unsigned int, string>>::const_iterator tmpBlocksIter;
    cout<<"@@@@@@ CN analyzies a new stripe @@@@@@"<<endl;
    for(tmpBlocksIter = tmpBlocks.begin(); tmpBlocksIter != tmpBlocks.end(); ++tmpBlocksIter){
      tmp_block_idx = (*tmpBlocksIter).first;
      tmp_block = (*tmpBlocksIter).second;
      tmp_IP = meta->getBlock2IP(tmp_block);
      temp_blocks[tmp_block_idx] = tmp_block;
      temp_IPs[tmp_block_idx] = tmp_IP;
      cout<<"block "<<tmp_block_idx<<", "<<tmp_block<<", IP: "<<tmp_IP<<endl;
    }

    cout<<"start upcode..."<<endl;
    gettimeofday(&start_time, NULL);

    char* gw_cmd = new char[400];
    gw_cmd[0] = '\0';

    for(int idx = k; idx < k + l_f; ++idx) {
      string cmd = generateUpcodeCmd(temp_blocks, temp_IPs, idx, gw_ip, gw_cmd);
      sendCmd(cmd, temp_IPs[idx]);
      cout<<"~~~~~~ send cmd to local parity block "<<(idx - k)<<" :"<<cmd<<endl;
    }

    if(gw_cmd[0] != '\0') {
      cout<<"gw "<<gw_ip<<", upcode cmd: "<<gw_cmd<<endl;
      sendCmd(string(gw_cmd), gw_ip);
    }
    delete gw_cmd;

    for(int i = 0; i < l_c; ++i) {
      ack_lens[i] = recvAck(acks[i]);
      cout<<"ack length: "<<ack_lens[i]<<endl;
    }
    bool finish_upcode = true;
    for(int i = 0; i < l_c; ++i) {
      if(strcmp(acks[i], "fi_upco") != 0) {
        finish_upcode = false;
        break;
      }
    }
    if(finish_upcode) {
      cout<<"@@@@@@ upcode success for stripe "<<tmp_stripe<<" !"<<endl;
      all_stripe_finish_tag[stripe_index] = true;
      gettimeofday(&end_time, NULL);
      upcode_time += end_time.tv_sec-start_time.tv_sec+(end_time.tv_usec-start_time.tv_usec)*1.0/1000000;
    } else {
      cout<<"@@@@@@ upcode error for stripe "<<tmp_stripe<<" xxxxxx"<<endl;
    }
    ++stripe_index;

  }// end of outer for

  bool all_stripe_finish_upcode = true;
  for(int i = 0; i < stripe_num; ++i) {
    if(all_stripe_finish_tag[i] == false) {
      all_stripe_finish_upcode = false;
      break;
    }
  }
  if(all_stripe_finish_upcode) {
    cout<<"@@@@@@ upcode success for file "<<file<<" !"<<endl;
    // update metadata when a file upcodes from fast code into compact code
    meta->upcodeUpdateMetadata(file);
    fprintf(stderr, "@@@@@@ upcode time: %.2lf s\n", upcode_time);
  }

  for(int i = 0; i < l_c; ++i){
    delete acks[i];
  }

  delete acks;
  delete ack_lens;
  delete all_stripe_finish_tag;
  tmpBlocks.clear();
  stripes.clear();
  return upcode_time;
}

  // test upcode command
void Coordinator::testUpcodeCmd(){
  //place_method = OPT_R;
  string stripe_blks[k + l_f];
  stripe_blks[0] = "data0";
  stripe_blks[1] = "data1";
  stripe_blks[2] = "data2";
  stripe_blks[3] = "data3";
  stripe_blks[4] = "lf0";
  stripe_blks[5] = "lf1";
  string blk_IPs[k + l_f];
  blk_IPs[0] = "192.168.0.22";
  blk_IPs[1] = "192.168.0.23";
  blk_IPs[2] = "192.168.0.26";
  blk_IPs[3] = "192.168.0.27";
  blk_IPs[4] = "192.168.0.24";
  blk_IPs[5] = "192.168.0.31";
  string gw_ip = meta->getGW();
  char* gw_cmd = new char[200];
  gw_cmd[0] = '\0';
  for(int blk_id = k; blk_id < k + l_f; ++blk_id) {
    cout<<"local parity block id"<<(blk_id - k)<<" , upcode cmd: "<<generateUpcodeCmd(stripe_blks, blk_IPs, blk_id, gw_ip, gw_cmd)<<endl;
  }
  if(gw_cmd[0] != '\0') {
    cout<<"gw "<<gw_ip<<", upcode cmd: "<<gw_cmd<<endl;
  }
  delete gw_cmd;
}

  // test upcode command when k = 12
void Coordinator::testUpcodeCmd_k_12(){
  //place_method = OPT_R;
  int k = 12;
  int l_f = 6;
  string stripe_blks[k + l_f];
  for(int i = 0; i < k; ++i) {
    stripe_blks[i] = "data" + to_string(i);
  }
  for(int i = k; i < k + l_f; ++i) {
    stripe_blks[i] = "lf" + to_string(i - k);
  }
  string blk_IPs[k + l_f];
  for(int i = 0; i < k + l_f; ++i) {
    blk_IPs[i] = "192.168.0." + to_string(i);
  }
  string gw_ip = meta->getGW();
  char* gw_cmd = new char[200];
  gw_cmd[0] = '\0';
  for(int blk_id = k; blk_id < k + l_f; ++blk_id) {
    cout<<"local parity block id"<<(blk_id - k)<<" , upcode cmd: "<<generateUpcodeCmd(stripe_blks, blk_IPs, blk_id, gw_ip, gw_cmd)<<endl;
  }
  if(gw_cmd[0] != '\0') {
    cout<<"gw "<<gw_ip<<", upcode cmd: "<<gw_cmd<<endl;
  }
  delete gw_cmd;
}

  /* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * 
   *                    generate commands for upcode                     *
   *                                                                     *
   * e.g., in Fig.4 of our infocom20 paper, L0' = L0 + L1 + L2,          *
   *                                                                     *
   * Opt-S: L1 and L2 will send blocks directly to L0 as they reside in  * 
   * the same rack/ cluster,                                             *
   *                                                                     *
   * Opt-R/ Flat: L1 and L2 will send blocks to the gateway,             *
   * which will re-send the blocks to L0 (cross-cluster)                 *
   * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
string Coordinator::generateUpcodeCmd(string stripe_blks[], string blk_IPs[], int fast_local_parity_id, string gw_ip, char* gw_cmd){
  //int k = 12; // (this is for testUpcodeCmd_k_12)
  //int l_f = 6; // (this is for testUpcodeCmd_k_12)
  //int l_c = 2; // (this is for testUpcodeCmd_k_12)
  string retCmd = "";
  if(!(k <= fast_local_parity_id && fast_local_parity_id < k + l_f)) {
    return retCmd;
  }

  retCmd += "up";

  int delta = l_f / l_c;
  string block = stripe_blks[fast_local_parity_id];
  string block_ip = blk_IPs[fast_local_parity_id];
  
  int compact_local_parity_id = k + (fast_local_parity_id - k) / delta;

  if((fast_local_parity_id - k) == (compact_local_parity_id - k) * delta) {
    string gw_waited_block_ip_concated_str = "";    

    // e.g., [L0/L0' in Fig.4]
    retCmd += "reco";
    retCmd += block;
    retCmd += "wa";
    int num_wait_blks = delta - 1;
    retCmd += to_string(num_wait_blks);
    retCmd += "blk";
    string tmp_blk;
    string tmp_ip;
    for(int idx = fast_local_parity_id + 1; idx <= fast_local_parity_id + delta - 1; ++idx) {
      tmp_blk = stripe_blks[idx];
      tmp_ip = blk_IPs[idx];
      if(place_method == OPT_S) {
        // in Opt-S, L0 waits for L1, L2, 
        // and L0, L1, and L2 are in the same rack/ cluster
        retCmd += tmp_ip;
      } else {
        // in Opt-R, or Flat, L0 waits for L1, L2, 
        // but L0, L1, and L2 reside in different racks/ clusters, 
        // so we wait blocks from the gateway
        retCmd += gw_ip;
        gw_waited_block_ip_concated_str += tmp_ip;
      }
    }

    if(place_method != OPT_S) {
      // gateway command, as stated above, only in Opt-R and Flat, 
      // the gateway will receive commands from the coordinator
      if(gw_cmd[0] == '\0') {
        string gw_cmd_str = "ga";
        gw_cmd_str += to_string(l_c);
        gw_cmd_str += "wa";
        gw_cmd_str += to_string(delta-1);
        gw_cmd_str += gw_waited_block_ip_concated_str;
        gw_cmd_str += "se";
        gw_cmd_str += block_ip;
        strcpy(gw_cmd, (char*)gw_cmd_str.c_str());
      } else {
        string gw_cmd_str = "wa";
        gw_cmd_str += to_string(delta-1);
        gw_cmd_str += gw_waited_block_ip_concated_str;
        gw_cmd_str += "se";
        gw_cmd_str += block_ip;
        strcat(gw_cmd, (char*)gw_cmd_str.c_str());
      }
    }

  } else {
    // e.g., [L1, L2 in Fig.4]
    int dest_fast_local_parity_id = (compact_local_parity_id - k) * delta + k;
    string dest_block = stripe_blks[dest_fast_local_parity_id];
    string dest_ip = blk_IPs[dest_fast_local_parity_id];
    if(place_method == OPT_S) {
      // in Opt-S, L1, L2 directly send blocks to L0
      retCmd += "se";
      retCmd += block;
      retCmd += dest_ip;
    } else {
      // in Opt-R and Flat, L1, L2 send blocks to the gateway, 
      // and the gateway re-sends blocks to L0
      retCmd += "se";
      retCmd += block;
      retCmd += gw_ip;
    }

  }

  return retCmd;
}

  /* * * * * * * * * * * * * * * * * * * *
   *   kernel routine 4: downcodeFile    *
   * * * * * * * * * * * * * * * * * * * */
double Coordinator::downcodeFile(string file){
  double downcode_time = 0.0;
  set<string> stripes = meta->getFile2Stripes(file);
  string tmp_stripe;
  set<pair<unsigned int, string>> tmpBlocks;
  unsigned int tmp_block_idx;
  string tmp_block;
  string tmp_IP;
  bool hot_tag = meta->isFileHot(file);
  if(hot_tag) {
    cout<<"file [ "<<file<<" ] is hot, cannot downcode"<<endl;
    return downcode_time;
  }
  int stripe_len = k + l_c + g;
  string temp_blocks[stripe_len];
  string temp_IPs[stripe_len];
  // reserved blocks are for example, [L1, L2, L4, L5 in Fig.4 in paper]
  int reserved_len = k + l_f;
  string reserved_blocks[reserved_len];
  string reserved_IPs[reserved_len];

  int ack_size = 1024;
  int ack_num = l_c;
  char** acks = new char*[ack_num];
  for(int i = 0; i < ack_num; ++i) {
    acks[i] = new char[ack_size];
  }
  int* ack_lens =  new int[ack_num];

  int stripe_num = stripes.size();
  bool* all_stripe_finish_tag = new bool[stripe_num];
  for(int i = 0; i < stripe_num; ++i) {
    all_stripe_finish_tag[i] = false;
  }
  int stripe_index = 0;

  string gw_ip = meta->getGW();

  struct timeval start_time, end_time;
  set<string>::const_iterator stripesIter;
  for(stripesIter = stripes.begin(); stripesIter != stripes.end(); ++stripesIter){
    tmp_stripe = *stripesIter;
    tmpBlocks = meta->getStripe2Blocks(tmp_stripe);
    set<pair<unsigned int, string>>::const_iterator tmpBlocksIter;
    cout<<"&&&&&& CN analyzies a new stripe &&&&&&"<<endl;
    for(tmpBlocksIter = tmpBlocks.begin(); tmpBlocksIter != tmpBlocks.end(); ++tmpBlocksIter){
      tmp_block_idx = (*tmpBlocksIter).first;
      tmp_block = (*tmpBlocksIter).second;
      tmp_IP = meta->getBlock2IP(tmp_block);
      temp_blocks[tmp_block_idx] = tmp_block;
      temp_IPs[tmp_block_idx] = tmp_IP;
      cout<<"block "<<tmp_block_idx<<", "<<tmp_block<<", IP: "<<tmp_IP<<endl;
    }

    // retrieve the reserved blocks
    tmpBlocks = meta->getStripe2ReservedBlocks(tmp_stripe);
    cout<<"-&-&-& reserved blocks &-&-&-"<<endl;
    for(tmpBlocksIter = tmpBlocks.begin(); tmpBlocksIter != tmpBlocks.end(); ++tmpBlocksIter){
      tmp_block_idx = (*tmpBlocksIter).first;
      tmp_block = (*tmpBlocksIter).second;
      tmp_IP = meta->getBlock2IP(tmp_block);
      reserved_blocks[tmp_block_idx] = tmp_block;
      reserved_IPs[tmp_block_idx] = tmp_IP;
      cout<<"reserved block "<<tmp_block_idx<<", "<<tmp_block<<", IP: "<<tmp_IP<<endl;
    }

    cout<<"start downcode..."<<endl;
    gettimeofday(&start_time, NULL);

    char* gw_cmd = new char[400];
    gw_cmd[0] = '\0';
    char* gw_cmd_further4flat = new char[100];
    gw_cmd_further4flat[0] = '\0';

    // [send commands to D0-D5, L0]
    for(int i = 0; i < k + l_c; ++i) {
      string cmd = generateDowncodeCmd(temp_blocks, temp_IPs, reserved_blocks, reserved_IPs, i, -1, gw_ip, gw_cmd, gw_cmd_further4flat);
      if(cmd != "") {
        sendCmd(cmd, temp_IPs[i]);
      }
      if(i < k) {
        cout<<"%%%%%% send cmd to data block "<<i<<" :"<<cmd<<endl;
      } else {
        cout<<"%%%%%% send cmd to compact local parity block "<<(i - k)<<" :"<<cmd<<endl;
      }
    }
    // [send commands to L1, L2]
    for(int i = k; i < k + l_f; ++i) {
      string cmd = generateDowncodeCmd(temp_blocks, temp_IPs, reserved_blocks, reserved_IPs, -1, i, gw_ip, gw_cmd, gw_cmd_further4flat);
      int delta = l_f / l_c;
      if((i - k) % delta != 0) {
        sendCmd(cmd, reserved_IPs[i]);
        cout<<"------ send cmd to fast local parity block "<<(i - k)<<" :"<<cmd<<endl;
      }
    }

    if(gw_cmd[0] != '\0') {
      strcat(gw_cmd, gw_cmd_further4flat);
      cout<<"gw "<<gw_ip<<", downcode cmd: "<<gw_cmd<<endl;
      sendCmd(string(gw_cmd), gw_ip);
    }
    delete gw_cmd;

    for(int i = 0; i < ack_num; ++i) {
      ack_lens[i] = recvAck(acks[i]);
      cout<<"ack length: "<<ack_lens[i]<<endl;
    }
    bool finish_downcode = true;
    for(int i = 0; i < ack_num; ++i) {
      if(strcmp(acks[i], "fi_doco") != 0) {
        finish_downcode = false;
        break;
      }
    }
    if(finish_downcode) {
      cout<<"%%%%%% downcode success for stripe "<<tmp_stripe<<" !"<<endl;
      all_stripe_finish_tag[stripe_index] = true;
      gettimeofday(&end_time, NULL);
      downcode_time += end_time.tv_sec-start_time.tv_sec+(end_time.tv_usec-start_time.tv_usec)*1.0/1000000;
    } else {
      cout<<"%%%%%% downcode error for stripe "<<tmp_stripe<<" xxxxxx"<<endl;
    }
    ++stripe_index;

  } // end of outer for

  bool all_stripe_finish_downcode = true;
  for(int i = 0; i < stripe_num; ++i) {
    if(all_stripe_finish_tag[i] == false) {
      all_stripe_finish_downcode = false;
      break;
    }
  }
  if(all_stripe_finish_downcode) {
    cout<<"%%%%%% downcode success for file "<<file<<" !"<<endl;
    // update metadata when a file downcodes from compact code into fast code
    meta->downcodeUpdateMetadata(file);
    fprintf(stderr, "%%%%%% downcode time: %.2lf s\n", downcode_time);
  }

  for(int i = 0; i < ack_num; ++i){
    delete acks[i];
  }

  delete acks;
  delete ack_lens;
  delete all_stripe_finish_tag;
  tmpBlocks.clear();
  stripes.clear();
  return downcode_time;
}

  // test downcode command, when k = 12
void Coordinator::testDowncodeCmd_k_12(){
  //place_method = Opt-R;
  int k = 12;
  int l_f = 6;
  int l_c = 2;
  string stripe_blks[k + l_c];
  for(int i = 0; i < k; ++i) {
    stripe_blks[i] = "data" + to_string(i);
  }
  for(int i = k; i < k + l_c; ++i) {
    stripe_blks[i] = "lc" + to_string(i - k);
  }
  string reserved_blks[k + l_f];
  for(int i = 0; i < k; ++i) {
    reserved_blks[i] = "NA_BLK";
  }
  for(int i = k; i < k + l_f; ++i) {
    reserved_blks[i] = "lf" + to_string(i - k);
  }
  
  string blk_IPs[k + l_c];
  string reserved_IPs[k + l_f];
  for(int i = 0; i < k; ++i) {
    blk_IPs[i] = "192.168.0." + to_string(i);
    reserved_IPs[i] = "NA_IP";
  }
  blk_IPs[k] = "192.168.0.12";
  reserved_IPs[k+1] = "192.168.0.13";
  reserved_IPs[k+2] = "192.168.0.14";
  blk_IPs[k+1] = "192.168.0.15";
  reserved_IPs[k+4] = "192.168.0.16";
  reserved_IPs[k+5] = "192.168.0.17";

  string gw_ip = meta->getGW();
  char* gw_cmd = new char[400];
  gw_cmd[0] = '\0';
  char* gw_cmd_further4flat = new char[100];
  gw_cmd_further4flat[0] = '\0';
  for(int blk_id = 0; blk_id < k + l_c; ++blk_id) {
    cout<<"blk_id"<<blk_id<<", downcode cmd: "<<generateDowncodeCmd(stripe_blks, blk_IPs, reserved_blks, reserved_IPs, blk_id, -1, gw_ip, gw_cmd, gw_cmd_further4flat)<<endl;
  }
  for(int blk_id = k; blk_id < k + l_f; ++blk_id) {
    cout<<"reserved_id"<<blk_id<<", downcode cmd: "<<generateDowncodeCmd(stripe_blks, blk_IPs, reserved_blks, reserved_IPs, -1, blk_id, gw_ip, gw_cmd, gw_cmd_further4flat)<<endl;
  }
  if(gw_cmd[0] != '\0') {
    strcat(gw_cmd, gw_cmd_further4flat);
    cout<<"gw "<<gw_ip<<", upcode cmd: "<<gw_cmd<<endl;
  }
  delete gw_cmd;
}

  /* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * 
   *                   generate commands for downcode                    *
   *                                                                     *
   * e.g., in Fig.4 of our infocom20 paper,                              *
   *                                                                     *
   * Opt-S:                                                              *
   *   1) D0 and D1 will send blocks directly to L0 as they reside in    * 
   * the same rack/ cluster,                                             *
   *                                                                     *
   *   2) D3 will send its content to D2. D2 will calculate an XOR sum of*
   * D2+D3, and then send D2+D3 to the gateway, which will re-send D2+D3 *
   * to L1 (cross-cluster),                                              *
   *                                                                     *
   *   3) L0 and L1 will send blocks to L0', which calculates L2 (L0,    *
   *  L1, L0' are all in the same core rack/ cluter,                     *
   *                                                                     *
   * Opt-R:                                                              *
   *   D0 and D1 send blocks inner-cluster to produce L0,                *
   *   D2 and D3 send blocks inner-cluster to produce L1,                *
   *   D4 and D5 send blocks inner-cluster to produce L2,                *
   *                                                                     *
   * Flat:                                                               *
   *   1) D0 and D1 send blocks to the gateway, which redirects to L0 (  *
   * cross-cluster),                                                     * 
   *   2) D2 and D3 send blocks to the gateway, which redirects to L1 (  *
   * cross-cluster),                                                     *
   *   3) L0 and L1 send blocks to the gateway, which redirects to L0' ( *
   * L2 = L0' + L0 + L2, cross-cluster)                                  *
   * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
string Coordinator::generateDowncodeCmd(string stripe_blks[], string blk_IPs[], string reserved_blks[], string reserved_IPs[], int blk_id, int reserved_id, string gw_ip, char* gw_cmd, char* gw_cmd_f){
  //int k = 12; // (this is for testDowncodeCmd_k_12)
  //int l_f = 6; // (this is for testDowncodeCmd_k_12)
  //int l_c = 2; // (this is for testDowncodeCmd_k_12)
  string retCmd = "";

  int r_c = k / l_c;
  int r_f = k / l_f;
  int delta = l_f / l_c;

  if(reserved_id == -1) {
    // for blocks D0-D5, L0
    if(0 <= blk_id && blk_id < k) {
      // data block
      if(place_method == OPT_S || place_method == FLAT) {
        int compact_local_group_id = blk_id / r_c;
        if(compact_local_group_id * r_c <= blk_id && blk_id < compact_local_group_id * r_c + (delta - 1) * g) {
        } else {
          return retCmd;
        }
      }
    } else if (blk_id < k + l_c) {
      // compact local parity block
    } else {
      // global parity block
      return retCmd;
    }
  }

  if(blk_id == -1) {
    // for blocks L1, L2
    if(k <= reserved_id && reserved_id < k + l_f) {
      int fast_local_group_id = reserved_id - k;
      if(fast_local_group_id % delta == 0) {
        return retCmd;
      } else {
      }
    } else {
      return retCmd;
    }
  }

  if(reserved_id == -1) {
    retCmd = generateDowncodeCmd4DataAndFastLP(stripe_blks, blk_IPs, reserved_blks, reserved_IPs, blk_id, gw_ip, gw_cmd);
  } // end of if(reserved_id == -1)

  if(blk_id == -1) {
    retCmd = generateDowncodeCmd4ReservedLP(stripe_blks, blk_IPs, reserved_blks, reserved_IPs, reserved_id, gw_ip, gw_cmd, gw_cmd_f);
  } // end of if(blk_id == -1)

  return retCmd;
}

  // generate downcode commands for D0-D5, L0
string Coordinator::generateDowncodeCmd4DataAndFastLP(string stripe_blks[], string blk_IPs[], string reserved_blks[], string reserved_IPs[], int blk_id, string gw_ip, char* gw_cmd) {
    int r_c = k / l_c;
    int r_f = k / l_f;
    int delta = l_f / l_c;

    string block;
    string block_ip;
    string reserved_block;
    string reserved_ip;
    string tmp_blk;
    string tmp_ip;
    string retCmd = "do";

    block = stripe_blks[blk_id];
    block_ip = blk_IPs[blk_id];
    if(0 <= blk_id && blk_id < k) {
      // data block
      int compact_local_group_id = blk_id / r_c;
      int fast_local_group_id = blk_id / r_f;
      int smallest_id_this_fast_local_group = fast_local_group_id * r_f;
      if(place_method == OPT_S && compact_local_group_id * r_c <= blk_id && blk_id < compact_local_group_id * r_c + (delta - 1) * g) {
        if(fast_local_group_id % delta == 0) {
          // in Opt-S, D0, D1, send blocks to L0
          int dest_parity_id = k + fast_local_group_id / delta;
          tmp_blk = stripe_blks[dest_parity_id];
          tmp_ip = blk_IPs[dest_parity_id];
          retCmd += "se";
          retCmd += block;
          retCmd += tmp_ip;
        } else {
          if(blk_id == smallest_id_this_fast_local_group) {
            // in Opt-S, D2 waits for D3, and re-sends an XOR sum (D2+D3) to the gateway
            retCmd += "wa";
            retCmd += to_string(r_f - 1);
            retCmd += "blk";
            for(int idx = smallest_id_this_fast_local_group + 1; idx < smallest_id_this_fast_local_group + r_f; ++idx) {
              tmp_blk = stripe_blks[idx];
              tmp_ip = blk_IPs[idx];
              retCmd += tmp_ip;
            }
            int reserved_parity_id = fast_local_group_id + k;
            tmp_blk = reserved_blks[reserved_parity_id];
            tmp_ip = reserved_IPs[reserved_parity_id];
            retCmd += "se";
            retCmd += block;
            retCmd += gw_ip;

            if(delta - 2 > 0) {
              // gateway command
              // the gateway re-sends D2+D3 to L1
              if(gw_cmd[0] == '\0') {
                string gw_cmd_str = "ga";
                gw_cmd_str += to_string((delta - 2) * l_c);
                gw_cmd_str += "wa";
                gw_cmd_str += to_string(1);
                gw_cmd_str += block_ip;
                gw_cmd_str += "se";
                gw_cmd_str += tmp_ip;
                strcpy(gw_cmd, (char*)gw_cmd_str.c_str());
              } else {
                string gw_cmd_str = "wa";
                gw_cmd_str += to_string(1);
                gw_cmd_str += block_ip;
                gw_cmd_str += "se";
                gw_cmd_str += tmp_ip;
                strcat(gw_cmd, (char*)gw_cmd_str.c_str());
              }
            }
          } else {
            // in Opt-S, D3 directly sends its content to D2
            tmp_blk = stripe_blks[smallest_id_this_fast_local_group];
            tmp_ip = blk_IPs[smallest_id_this_fast_local_group];
            retCmd += "se";
            retCmd += block;
            retCmd += tmp_ip;
          }
        }
      } // place_method == OPT_S

      if(place_method == OPT_R || (place_method == FLAT && compact_local_group_id * r_c <= blk_id && blk_id < compact_local_group_id * r_c + (delta - 1) * g)) {
        if(fast_local_group_id % delta == 0) {
          int dest_parity_id = k + fast_local_group_id / delta;
          tmp_blk = stripe_blks[dest_parity_id];
          tmp_ip = blk_IPs[dest_parity_id];
          retCmd += "se";
          retCmd += block;
          if(place_method == OPT_R) {
            // in Opt-R, D0, D1, send blocks to L0
            retCmd += tmp_ip;
          } else {
            // in Flat, D0, D1, send blocks to the gateway
            retCmd += gw_ip;
          }
        } else {
          int reserved_parity_id = fast_local_group_id + k;
          tmp_blk = reserved_blks[reserved_parity_id];
          tmp_ip = reserved_IPs[reserved_parity_id];
          retCmd += "se";
          retCmd += block;
          if(place_method == OPT_R) {
            // in Opt-R, D2, D3, send blocks to L1
            // in Opt-R, D4, D5, send blocks to L2
            retCmd += tmp_ip;
          } else {
            // in Flat, D2, D3, send blocks to the gateway
            retCmd += gw_ip;
          }
        }
      } // place_method == OPT_R || place_method == FLAT

    } else if (blk_id < k + l_c) {
      // compact local parity block
      retCmd += "lp";
      int compact_local_group_id = blk_id - k;
      int fast_local_group_id = compact_local_group_id * delta;
      retCmd += "wa";
      retCmd += to_string(r_f);
      retCmd += "blk";

      string gw_waited_block_ip_concated_str = "";
      for(int idx = fast_local_group_id * r_f; idx < fast_local_group_id * r_f + r_f; ++idx) {
        tmp_blk = stripe_blks[idx];
        tmp_ip = blk_IPs[idx];
        if(place_method != FLAT) {
          // in Opt-S and Opt-R, L0 waits for D0, D1
          retCmd += tmp_ip;
        } else {
          // in Flat, L0 waits for blocks from the gateway
          retCmd += gw_ip;
          gw_waited_block_ip_concated_str += tmp_ip;
        }
      }

      if(place_method == FLAT) {
        // in Flat, gateway command
        if(gw_cmd[0] == '\0') {
          string gw_cmd_str = "ga";
          gw_cmd_str += to_string(l_f - l_c);
          gw_cmd_str += "wa";
          gw_cmd_str += to_string(r_f);
          gw_cmd_str += gw_waited_block_ip_concated_str;
          gw_cmd_str += "se";
          gw_cmd_str += block_ip;
          strcpy(gw_cmd, (char*)gw_cmd_str.c_str());
        } else {
          string gw_cmd_str = "wa";
          gw_cmd_str += to_string(r_f);
          gw_cmd_str += gw_waited_block_ip_concated_str;
          gw_cmd_str += "se";
          gw_cmd_str += block_ip;
          strcat(gw_cmd, (char*)gw_cmd_str.c_str());
        }
      }

      if(place_method == OPT_S || place_method == FLAT) {
        // in Opt-S and Flat, L0 shoud be redirected to L2
        retCmd += "st";
        retCmd += "re";
        int reserved_parity_id = fast_local_group_id + delta - 1 + k;
        tmp_blk = reserved_blks[reserved_parity_id];
        tmp_ip = reserved_IPs[reserved_parity_id];
        retCmd += "se";
        retCmd += block;
        if(place_method == OPT_S) {
          retCmd += tmp_ip;
        } else {
          retCmd += gw_ip;
        }
      } else if (place_method == OPT_R) {
        retCmd += "castfi";
        retCmd += block;
      }
    }

    return retCmd;
}

  // generate downcode commands for L1, L2
string Coordinator::generateDowncodeCmd4ReservedLP(string stripe_blks[], string blk_IPs[], string reserved_blks[], string reserved_IPs[], int reserved_id, string gw_ip, char* gw_cmd, char* gw_cmd_f) {
    int r_c = k / l_c;
    int r_f = k / l_f;
    int delta = l_f / l_c;

    string block;
    string block_ip;
    string reserved_block;
    string reserved_ip;
    string tmp_blk;
    string tmp_ip;
    string retCmd = "do";

    reserved_block = reserved_blks[reserved_id];
    reserved_ip = reserved_IPs[reserved_id];
    retCmd += "lp";
    int fast_local_group_id = reserved_id - k;
    int id = (fast_local_group_id / delta) * delta;
    int id2 = id + delta - 1;
    
    if(place_method == OPT_S && fast_local_group_id < id2) {
      // in Opt-S, L1
      retCmd += "wa";
      retCmd += to_string(1);
      retCmd += "blk";
      int data_block_id = fast_local_group_id * r_f;
      tmp_blk = stripe_blks[data_block_id];
      tmp_ip = blk_IPs[data_block_id];
      retCmd += gw_ip;

      retCmd += "st";
      retCmd += "de";
      int reserved_parity_id = id2 + k;
      tmp_blk = reserved_blks[reserved_parity_id];
      tmp_ip = reserved_IPs[reserved_parity_id];
      retCmd += "se";
      retCmd += reserved_block;
      retCmd += tmp_ip;
    } else if (place_method == OPT_S &&fast_local_group_id == id2) {
      // in Opt-S, L2
      retCmd += "wa";
      retCmd += to_string(delta - 1);
      retCmd += "blk";
      int start_parity_id = id / delta + k;
      tmp_blk = stripe_blks[start_parity_id];
      tmp_ip = blk_IPs[start_parity_id];
      retCmd += tmp_ip;
      for(int idx = id + 1 + k; idx < id2 + k; ++idx) {
        tmp_blk = reserved_blks[idx];
        tmp_ip = reserved_IPs[idx];
        retCmd += tmp_ip;
      }
      retCmd += "castfi";
      retCmd += reserved_block;
    } // end of if place_method = OPT_S 
    else if (place_method == OPT_R) {
      // in Opt-R, L1, L2
      retCmd += "wa";
      retCmd += to_string(r_f);
      retCmd += "blk";
      int start_data_block_id = fast_local_group_id * r_f;
      for(int idx = start_data_block_id; idx < start_data_block_id + r_f; ++idx) {
        tmp_blk = stripe_blks[idx];
        tmp_ip = blk_IPs[idx];
        retCmd += tmp_ip;
      }
      retCmd += "castfi";
      retCmd += reserved_block;
    } // end of if place_method == OPT_R
    else if (place_method == FLAT && fast_local_group_id < id2) {
      // in Flat, L1
      retCmd += "wa";
      retCmd += to_string(r_f);
      retCmd += "blk";
      int start_data_block_id = fast_local_group_id * r_f;

      string gw_waited_block_ip_concated_str = "";
      for(int idx = start_data_block_id; idx < start_data_block_id + r_f; ++idx) {
        tmp_blk = stripe_blks[idx];
        tmp_ip = blk_IPs[idx];
        retCmd += gw_ip;
        gw_waited_block_ip_concated_str += tmp_ip;
      }

      if(gw_cmd[0] == '\0') {
        string gw_cmd_str = "ga";
        gw_cmd_str += to_string(l_f - l_c);
        gw_cmd_str += "wa";
        gw_cmd_str += to_string(r_f);
        gw_cmd_str += gw_waited_block_ip_concated_str;
        gw_cmd_str += "se";
        gw_cmd_str += reserved_ip;
        strcpy(gw_cmd, (char*)gw_cmd_str.c_str());
      } else {
        string gw_cmd_str = "wa";
        gw_cmd_str += to_string(r_f);
        gw_cmd_str += gw_waited_block_ip_concated_str;
        gw_cmd_str += "se";
        gw_cmd_str += reserved_ip;
        strcat(gw_cmd, (char*)gw_cmd_str.c_str());
      }

      retCmd += "st";
      retCmd += "de";
      int reserved_parity_id = id2 + k;
      tmp_blk = reserved_blks[reserved_parity_id];
      tmp_ip = reserved_IPs[reserved_parity_id];
      retCmd += "se";
      retCmd += reserved_block;
      retCmd += gw_ip;
    } 
    else if (place_method == FLAT && fast_local_group_id == id2) {
      // in Flat, L2
      retCmd += "wa";
      retCmd += to_string(delta - 1);
      retCmd += "blk";

      string gw_waited_block_ip_concated_str = "";
      int start_parity_id = id / delta + k;
      tmp_blk = stripe_blks[start_parity_id];
      tmp_ip = blk_IPs[start_parity_id];
      retCmd += gw_ip;
      gw_waited_block_ip_concated_str += tmp_ip;
      for(int idx = id + 1 + k; idx < id2 + k; ++idx) {
        tmp_blk = reserved_blks[idx];
        tmp_ip = reserved_IPs[idx];
        retCmd += gw_ip;
        gw_waited_block_ip_concated_str += tmp_ip;
      }
      
      if(gw_cmd_f[0] == '\0') {
        string gw_cmd_str = to_string(l_c);
        gw_cmd_str += "wa";
        gw_cmd_str += to_string(delta - 1);
        gw_cmd_str += gw_waited_block_ip_concated_str;
        gw_cmd_str += "se";
        gw_cmd_str += reserved_ip;
        strcpy(gw_cmd_f, (char*)gw_cmd_str.c_str());
      } else {
        string gw_cmd_str = "wa";
        gw_cmd_str += to_string(delta - 1);
        gw_cmd_str += gw_waited_block_ip_concated_str;
        gw_cmd_str += "se";
        gw_cmd_str += reserved_ip;
        strcat(gw_cmd_f, (char*)gw_cmd_str.c_str());
      }
      
      retCmd += "castfi";
      retCmd += reserved_block;
    } // end of if place_method == FLAT

    return retCmd;
}
