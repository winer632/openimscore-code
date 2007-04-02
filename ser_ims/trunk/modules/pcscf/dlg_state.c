/**
 * $Id$
 *  
 * Copyright (C) 2004-2006 FhG Fokus
 *
 * This file is part of Open IMS Core - an open source IMS CSCFs & HSS
 * implementation
 *
 * Open IMS Core is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * For a license to use the Open IMS Core software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact Fraunhofer FOKUS by e-mail at the following
 * addresses:
 *     info@open-ims.org
 *
 * Open IMS Core is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * It has to be noted that this Open Source IMS Core System is not 
 * intended to become or act as a product in a commercial context! Its 
 * sole purpose is to provide an IMS core reference implementation for 
 * IMS technology testing and IMS application prototyping for research 
 * purposes, typically performed in IMS test-beds.
 * 
 * Users of the Open Source IMS Core System have to be aware that IMS
 * technology may be subject of patents and licence terms, as being 
 * specified within the various IMS-related IETF, ITU-T, ETSI, and 3GPP
 * standards. Thus all Open IMS Core users have to take notice of this 
 * fact and have to agree to check out carefully before installing, 
 * using and extending the Open Source IMS Core System, if related 
 * patents and licences may become applicable to the intended usage 
 * context.  
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * 
 */
 
/**
 * \file
 * 
 * Proxy-CSCF - Dialog State
 * 
 *  \author Dragos Vingarzan vingarzan -at- fokus dot fraunhofer dot de
 * 
 */
 
#include <time.h>

#include "dlg_state.h"

#include "../../mem/shm_mem.h"

#include "sip.h"

int p_dialogs_hash_size;					/**< size of the dialogs hash table 	*/
p_dialog_hash_slot *p_dialogs=0;			/**< the dialogs hash table				*/

extern int pcscf_dialogs_expiration_time;	/**< expiration time for a dialog		*/

time_t d_time_now;							/**< current time for dialog updates 	*/

extern str pcscf_record_route_mo;			/**< Record-route for originating case 	*/
extern str pcscf_record_route_mo_uri;		/**< URI for Record-route originating	*/ 
extern str pcscf_record_route_mt;			/**< Record-route for terminating case 	*/
extern str pcscf_record_route_mt_uri;		/**< URI for Record-route terminating	*/

extern str pcscf_name_str;					/**< fixed SIP URI of this P-CSCF 		*/

/**
 * Computes the hash for a string.
 * @param call_id - input string
 * @returns - the hash
 */
inline unsigned int get_p_dialog_hash(str call_id)
{
#define h_inc h+=v^(v>>3)
   char* p;
   register unsigned v;
   register unsigned h;

   h=0;
   for (p=call_id.s; p<=(call_id.s+call_id.len-4); p+=4){
       v=(*p<<24)+(p[1]<<16)+(p[2]<<8)+p[3];
       h_inc;
   }
   v=0;
   for (;p<(call_id.s+call_id.len); p++) {
       v<<=8;
       v+=*p;
   }
   h_inc;

   h=((h)+(h>>11))+((h>>13)+(h>>23));
   return (h)%p_dialogs_hash_size;
#undef h_inc 
}

/**
 * Initialize the registrar.
 * @param hash_size - the number of hash table cells
 * @returns 1 on success, 0 on error
 */
int p_dialogs_init(int hash_size)
{
	int i;
	
	p_dialogs_hash_size = hash_size;
	p_dialogs = shm_malloc(sizeof(p_dialog_hash_slot)*p_dialogs_hash_size);

	if (!p_dialogs) return 0;

	memset(p_dialogs,0,sizeof(p_dialog_hash_slot)*p_dialogs_hash_size);
	
	for(i=0;i<p_dialogs_hash_size;i++){
		p_dialogs[i].lock = lock_alloc();
		if (!p_dialogs[i].lock){
			LOG(L_ERR,"ERR:"M_NAME":d_hash_table_init(): Error creating lock\n");
			return 0;
		}
		p_dialogs[i].lock = lock_init(p_dialogs[i].lock);
	}
			
	return 1;
}

/**
 * Destroy the registrar.
 */
void p_dialogs_destroy()
{
	int i;
	p_dialog *d,*nd;
	for(i=0;i<p_dialogs_hash_size;i++){
		d_lock(i);
			d = p_dialogs[i].head;
			while(d){
				nd = d->next;
				free_p_dialog(d);
				d = nd;
			}
		d_unlock(i);
		lock_dealloc(p_dialogs[i].lock);
	}
	shm_free(p_dialogs);
}

/**
 * Locks the required part of the hash table.
 * @param hash - hash of the element to lock (hash slot number)
 */
inline void d_lock(unsigned int hash)
{
//	LOG(L_CRIT,"GET %d\n",hash);
	lock_get(p_dialogs[(hash)].lock);
//	LOG(L_CRIT,"GOT %d\n",hash);	
}

/**
 * UnLocks the required part of the hash table.
 * @param hash - hash of the element to lock (hash slot number)
 */
 inline void d_unlock(unsigned int hash)
{
	lock_release(p_dialogs[(hash)].lock);
//	LOG(L_CRIT,"RELEASED %d\n",hash);	
}

/**
 * Actualize the current time.
 * @returns the current time
 */
inline int d_act_time()
{
	d_time_now=time(0);
	return d_time_now;
}


/**
 * Creates a new p_dialog structure.
 * Does not add the structure to the list.
 * @param call_id - call-id of the dialog
 * @param host - host that originates/terminates this dialog
 * @param port - port that originates/terminates this dialog
 * @param transport - transport that originates/terminates this dialog
 * @returns the new p_dialog* on success, or NULL on error;
 */
p_dialog* new_p_dialog(str call_id,str host,int port, int transport)
{
	p_dialog *d;
	
	d = shm_malloc(sizeof(p_dialog));
	if (!d) {
		LOG(L_ERR,"ERR:"M_NAME":new_p_dialog(): Unable to alloc %d bytes\n",
			sizeof(p_dialog));
		goto error;
	}
	memset(d,0,sizeof(p_dialog));
	
	d->hash = get_p_dialog_hash(call_id);		
	STR_SHM_DUP(d->call_id,call_id,"shm");
	STR_SHM_DUP(d->host,host,"shm");	
	d->port = port;
	d->transport = transport;
				
	return d;
error:
	if (d){
		shm_free(d);		
	}
	return 0;
}

/**
 * Creates and adds a p_dialog to the hash table.
 * \note Locks the hash slot if ok! Call d_unlock(p_dialog->hash) when you are finished)
 * \note make sure that is_p_dialog(call_id) returns 0 or there will be unreachable duplicates!
 * @param call_id - dialog's call_id
 * @param host - host that originates/terminates this dialog
 * @param port - port that originates/terminates this dialog
 * @param transport - transport that originates/terminates this dialog
 * @returns the new p_dialog* on success, or NULL on error;
 */
p_dialog* add_p_dialog(str call_id,str host,int port, int transport)
{
	p_dialog *d;
	
	d = new_p_dialog(call_id,host,port,transport);
	if (!d) return 0;		
	
	d_lock(d->hash);
		d->next = 0;
		d->prev = p_dialogs[d->hash].tail;
		if (d->prev) d->prev->next = d;
		p_dialogs[d->hash].tail = d;
		if (!p_dialogs[d->hash].head) p_dialogs[d->hash].head = d;

		return d;
}

/**
 * Finds out if a dialog is in the hash table.
 * @param call_id - dialog's call_id
 * @param host - host that originates/terminates this dialog
 * @param port - port that originates/terminates this dialog
 * @param transport - transport that originates/terminates this dialog
 * @returns - 1 if the dialog exists, 0 if not
 * \note transport is ignored.
 */
int is_p_dialog(str call_id,str host,int port, int transport)
{
	p_dialog *d=0;
	unsigned int hash = get_p_dialog_hash(call_id);

	d_lock(hash);
		d = p_dialogs[hash].head;
		while(d){
			if (d->port == port &&
/*				d->transport == transport &&*/
/* commented because of strange behaviour */
				d->host.len == host.len &&
				d->call_id.len == call_id.len &&
				strncasecmp(d->host.s,host.s,host.len)==0 &&
				strncasecmp(d->call_id.s,call_id.s,call_id.len)==0) {
					d_unlock(hash);
					return 1;
				}
			d = d->next;
		}
	d_unlock(hash);
	return 0;
}

/**
 * Finds and returns a dialog from the hash table.
 * \note Locks the hash slot if ok! Call d_unlock(p_dialog->hash) when you are finished)
 * @param call_id - dialog's call_id
 * @param host - host that originates/terminates this dialog
 * @param port - port that originates/terminates this dialog
 * @param transport - transport that originates/terminates this dialog
 * \note transport is ignored.
 */
p_dialog* get_p_dialog(str call_id,str host,int port, int transport)
{
	p_dialog *d=0;
	unsigned int hash = get_p_dialog_hash(call_id);

	d_lock(hash);
		d = p_dialogs[hash].head;
		while(d){
			if (d->port == port &&
/*				d->transport == transport &&*/
/* commented because of strange behaviour */
				d->host.len == host.len &&
				d->call_id.len == call_id.len &&
				strncasecmp(d->host.s,host.s,host.len)==0 &&
				strncasecmp(d->call_id.s,call_id.s,call_id.len)==0) {
					return d;
				}
			d = d->next;
		}
	d_unlock(hash);
	return 0;
}

/**
 * Deletes and destroys the dialog from the hash table.
 * \note Must be called with a lock on the dialogs slot
 * @param d - the dialog to delete
 */
void del_p_dialog(p_dialog *d)
{
	if (d->prev) d->prev->next = d->next;
	else p_dialogs[d->hash].head = d->next;
	if (d->next) d->next->prev = d->prev;
	else p_dialogs[d->hash].tail = d->prev;
	free_p_dialog(d);
}

/**
 * Destroys the dialog.
 * @param d - the dialog to delete
 */
void free_p_dialog(p_dialog *d)
{
	int i;
	if (!d) return;
	if (d->call_id.s) shm_free(d->call_id.s);
	if (d->host.s) shm_free(d->host.s);	
	if (d->method_str.s) shm_free(d->method_str.s);	
	if (d->routes){
		for(i=0;i<d->routes_cnt;i++)
			shm_free(d->routes[i].s);
		shm_free(d->routes);
	}
	shm_free(d);
}

/**
 * Prints the dialog hash table.
 * @param log_level - the log_level to print with
 */
void print_p_dialogs(int log_level)
{
	p_dialog *d;
	int i,j;
	d_act_time();
	LOG(log_level,"INF:"M_NAME":----------  P-CSCF Dialog List begin --------------\n");
	for(i=0;i<p_dialogs_hash_size;i++){
		d_lock(i);
			d = p_dialogs[i].head;
			while(d){
				LOG(log_level,"INF:"M_NAME":[%4d] Call-ID:<%.*s>\t%d://%.*s:%d\tMet:[%d]\tState:[%d] Exp:[%4d]\n",i,				
					d->call_id.len,d->call_id.s,
					d->transport,d->host.len,d->host.s,d->port,
					d->method,d->state,
					(int)(d->expires - d_time_now));
				for(j=0;j<d->routes_cnt;j++)
					LOG(log_level,"INF:"M_NAME":\t RR: <%.*s>\n",			
						d->routes[j].len,d->routes[j].s);
					
				d = d->next;
			} 		
		d_unlock(i);
	}
	LOG(log_level,"INF:"M_NAME":----------  P-CSCF Dialog List end   --------------\n");	
}

/**
 * Finds the contact host/port/transport for a dialog.
 * @param msg - the SIP message to look into
 * @param direction - look for originating or terminating contact ("orig"/"term")
 * @param host - host to fill with the result
 * @param port - port to fill with the results
 * @param transport - transport to fill with the results
 * @returns 1 if found, 0 if not
 */
static inline int find_dialog_contact(struct sip_msg *msg,char *direction,str *host,int *port,int *transport)
{
	if (!direction) return 0;
	switch(direction[0]){
		case 'o':
		case 'O':
		case '0':
			if (!cscf_get_originating_contact(msg,host,port,transport))
				return 0;
			if (*port==0) *port = 5060;
			return 1;
		case 't':
		case 'T':
		case '1':
			if (!cscf_get_terminating_contact(msg,host,port,transport))
				return 0;
			if (*port==0) *port = 5060;
			return 1;
		default:
			LOG(L_CRIT,"ERR:"M_NAME":find_dialog_contact(): Unknown direction %s",direction);
			return 0;
	}
	return 1;
}

/**
 * Find out if a message is within a saved dialog.
 * @param msg - the SIP message
 * @param str1 - the direction of the dialog ("orig"/"term")
 * @param str2 - not used
 * @returns #CSCF_RETURN_TRUE if in, #CSCF_RETURN_FALSE else or #CSCF_RETURN_BREAK on error
 */
int P_is_in_dialog(struct sip_msg* msg, char* str1, char* str2)
{
	str call_id;
	str host;
	int port,transport;

	if (!find_dialog_contact(msg,str1,&host,&port,&transport)){
		LOG(L_ERR,"ERR:"M_NAME":P_is_in_dialog(%s): Error retrieving %s contact\n",str1,str1);
		return CSCF_RETURN_BREAK;
	}		

	//print_p_dialog(L_ERR);
	call_id = cscf_get_call_id(msg,0);
	if (!call_id.len)
		return CSCF_RETURN_FALSE;
	
//	d = get_p_dialog(call_id,host,port,transport);
//	if (!d){
//		/* if no dialog found, get out now */
//		return CSCF_RETURN_FALSE;
//	}
//	d_unlock(d->hash);
//	return CSCF_RETURN_TRUE;
		
	if (is_p_dialog(call_id,host,port,transport)) {
		return CSCF_RETURN_TRUE;
	}
	else 
		return CSCF_RETURN_FALSE;
}


str s_INVITE={"INVITE",6};
str s_SUBSCRIBE={"SUBSCRIBE",9};
/**
 * Return p_dialog_method for a method string.
 * @param method - the string containing the method
 * @returns the p_dialog_method corresponding if known or #DLG_METHOD_OTHER if not
 */
static enum p_dialog_method get_dialog_method(str method)
{
	if (method.len == s_INVITE.len &&
		strncasecmp(method.s,s_INVITE.s,s_INVITE.len)==0) return DLG_METHOD_INVITE;
	if (method.len == s_SUBSCRIBE.len &&
		strncasecmp(method.s,s_SUBSCRIBE.s,s_SUBSCRIBE.len)==0) return DLG_METHOD_SUBSCRIBE;
	return DLG_METHOD_OTHER;
}
	


/**
 * Saves a dialog.
 * @param msg - the initial request
 * @param str1 - direction - "orig" or "term"
 * @param str2 - not used
 * @returns #CSCF_RETURN_TRUE if ok, #CSCF_RETURN_FALSE if not or #CSCF_RETURN_BREAK on error 
 */
int P_save_dialog(struct sip_msg* msg, char* str1, char* str2)
{
	str call_id;
	p_dialog *d;
	str host;
	int port,transport;
	
	if (!find_dialog_contact(msg,str1,&host,&port,&transport)){
		LOG(L_ERR,"ERR:"M_NAME":P_is_in_dialog(): Error retrieving %s contact\n",str1);
		return CSCF_RETURN_BREAK;
	}		
		
	call_id = cscf_get_call_id(msg,0);
	if (!call_id.len)
		return CSCF_RETURN_FALSE;

	LOG(L_INFO,"DBG:"M_NAME":P_save_dialog(%s): Call-ID <%.*s>\n",str1,call_id.len,call_id.s);

	if (is_p_dialog(call_id,host,port,transport)){
		LOG(L_ERR,"ERR:"M_NAME":P_save_dialog: dialog already exists!\n");	
		return CSCF_RETURN_FALSE;
	}
	
	d = add_p_dialog(call_id,host,port,transport);
	if (!d) return CSCF_RETURN_FALSE;

	d->method = get_dialog_method(msg->first_line.u.request.method);
	STR_SHM_DUP(d->method_str,msg->first_line.u.request.method,"shm");
	d->first_cseq = cscf_get_cseq(msg,0);
	d->last_cseq = d->first_cseq;
	d->state = DLG_STATE_INITIAL;
	d->expires = d_act_time()+60;
	

	d_unlock(d->hash);
	
	print_p_dialogs(L_INFO);
	
	return CSCF_RETURN_TRUE;	
}

/**
 * Save the Record-routes for a dialog.
 * @param msg - the SIP message to extract RRs from
 * @param str1 - the direction to know if to reverse the RR list or not
 * @param d - dialog to save to
 */
void save_dialog_routes(struct sip_msg* msg, char* str1,p_dialog *d)
{
	int i;
	rr_t *rr,*ri;
	struct hdr_field *hdr;
	if (d->routes){
		for(i=0;i<d->routes_cnt;i++)
			shm_free(d->routes[i].s);
		shm_free(d->routes);
		d->routes = 0;
	}
	d->routes_cnt = 0;
	for(hdr=cscf_get_next_record_route(msg,0);hdr;hdr=cscf_get_next_record_route(msg,hdr)){
		rr = (rr_t*)hdr->parsed;
		for(ri=rr;ri;ri=ri->next)
			d->routes_cnt++;
	}
	d->routes = shm_malloc(sizeof(str)*d->routes_cnt);
	if (!d->routes){
		LOG(L_ERR,"ERR:"M_NAME":save_dialog_routes(): Unable to alloc %d bytes\n",
			sizeof(str)*d->routes_cnt);
		d->routes_cnt = 0;
		return;		
	}
	if (!str1) return;
	if (str1[0]=='o'||str1[0]=='0'||str1[0]=='O'){
		/* originating - reverse order */
		i = d->routes_cnt-1;
		for(hdr=cscf_get_next_record_route(msg,0);hdr;hdr=cscf_get_next_record_route(msg,hdr)){
			rr = (rr_t*)hdr->parsed;
			for(ri=rr;ri;ri=ri->next){
				STR_SHM_DUP(d->routes[i],ri->nameaddr.uri,"shm");
				i--;
			}
		}
	}else{
		/* terminating - normal order */
		i = 0;
		for(hdr=cscf_get_next_record_route(msg,0);hdr;hdr=cscf_get_next_record_route(msg,hdr)){
			rr = (rr_t*)hdr->parsed;
			for(ri=rr;ri;ri=ri->next){
				STR_SHM_DUP(d->routes[i],ri->nameaddr.uri,"shm");
				i++;
			}
		}
	}		
}

/**
 * Updates a dialog.
 * If the initial request was:
 * - INVITE - refreshes the expiration or looks for the BYE and destroys the dialog 
 * if found
 * - SUBSCRIBE - looks for the Subscription-state in NOTIFY, refreshes the expiration 
 * and if terminated destroys the dialog
 * - When adding more dialogs, add the refreshal methods here or they will expire and will
 * be destroyed. Also add the termination to reduce the memory consumption and improve the
 * performance.
 * @param msg - the request/response
 * @param str1 - direction - "orig" or "term"
 * @param str2 - not used
 * @returns #CSCF_RETURN_TRUE if ok, #CSCF_RETURN_FALSE if not or #CSCF_RETURN_BREAK on error 
 */
int P_update_dialog(struct sip_msg* msg, char* str1, char* str2)
{
	str call_id;
	p_dialog *d;
	int response;
	int cseq;
	struct hdr_field *h=0;
	struct sip_msg *req=0;
	str host;
	int port,transport;
	int expires;
	
	if (!find_dialog_contact(msg,str1,&host,&port,&transport)){
		LOG(L_ERR,"ERR:"M_NAME":P_update_dialog(%s): Error retrieving %s contact\n",str1,str1);
		return CSCF_RETURN_BREAK;
	}		
		
	call_id = cscf_get_call_id(msg,0);
	if (!call_id.len)
		return CSCF_RETURN_FALSE;

	LOG(L_INFO,"DBG:"M_NAME":P_update_dialog(%s): Call-ID <%.*s>\n",str1,call_id.len,call_id.s);

	d = get_p_dialog(call_id,host,port,transport);
	if (!d && msg->first_line.type==SIP_REPLY){
		/* Try to get the dialog from the request */
		req = cscf_get_request_from_reply(msg);		
		if (!find_dialog_contact(req,str1,&host,&port,&transport)){
			LOG(L_ERR,"ERR:"M_NAME":P_update_dialog(%s): Error retrieving %s contact\n",str1,str1);
			return CSCF_RETURN_BREAK;
		}		
		d = get_p_dialog(call_id,host,port,transport);		
	}
	if (!d){
		LOG(L_CRIT,"ERR:"M_NAME":P_update_dialog: dialog does not exists!\n");	
		return CSCF_RETURN_FALSE;
	}


	if (msg->first_line.type==SIP_REQUEST){
		/* Request */
		LOG(L_DBG,"DBG:"M_NAME":P_update_dialog(%s): Method <%.*s> \n",str1,
			msg->first_line.u.request.method.len,msg->first_line.u.request.method.s);
		cseq = cscf_get_cseq(msg,&h);
		if (cseq>d->last_cseq) d->last_cseq = cseq;
		d->expires = d_act_time()+pcscf_dialogs_expiration_time;
	}else{
		/* Reply */
		response = msg->first_line.u.reply.statuscode;
		LOG(L_DBG,"DBG:"M_NAME":P_update_dialog(%s): <%d> \n",str1,response);
		cseq = cscf_get_cseq(msg,&h);
		if (cseq==0 || h==0) return CSCF_RETURN_FALSE;
		if (d->first_cseq==cseq && d->method_str.len == ((struct cseq_body *)h->parsed)->method.len &&
			strncasecmp(d->method_str.s,((struct cseq_body *)h->parsed)->method.s,d->method_str.len)==0 &&
			d->state < DLG_STATE_CONFIRMED){
			/* reply to initial request */
			if (response<200){
				save_dialog_routes(msg,str1,d);
				d->state = DLG_STATE_EARLY;
				d->expires = d_act_time()+300;
			}else
			if (response>=200 && response<300){
				save_dialog_routes(msg,str1,d);
				d->state = DLG_STATE_CONFIRMED;
				d->expires = d_act_time()+pcscf_dialogs_expiration_time;
			}else
				if (response>300){
					d->state = DLG_STATE_TERMINATED;
					d_unlock(d->hash);				
					return P_drop_dialog(msg,str1,str2);
				}				
		}else{
			/* reply to subsequent request */			
			if (!req) req = cscf_get_request_from_reply(msg);
			
			/* destroy dialogs on specific methods */
			switch (d->method){
				case DLG_METHOD_OTHER:							
					break;
				case DLG_METHOD_INVITE:
					if (req && req->first_line.u.request.method.len==3 &&
						strncasecmp(req->first_line.u.request.method.s,"BYE",3)==0){
						d->state = DLG_STATE_TERMINATED;
						d_unlock(d->hash);				
						return P_drop_dialog(msg,str1,str2);
					}
					break;
				case DLG_METHOD_SUBSCRIBE:
//					if (req && req->first_line.u.request.method.len==9 &&
//						strncasecmp(req->first_line.u.request.method.s,"SUBSCRIBE",9)==0 &&
//						cscf_get_expires_hdr(msg)==0){						
//						d->state = DLG_STATE_TERMINATED;
//						d_unlock(d->hash);				
//						return P_drop_dialog(msg,str1,str2);
//					}
					if (req && req->first_line.u.request.method.len==6 &&
						strncasecmp(req->first_line.u.request.method.s,"NOTIFY",6)==0){
						expires = cscf_get_subscription_state(req);
						if (expires==0){						
							d->state = DLG_STATE_TERMINATED;
							d_unlock(d->hash);				
							return P_drop_dialog(msg,str1,str2);
						}else if (expires>0){
							d->expires = expires;
						}
					}
					break;
			}
			if (cseq>d->last_cseq) d->last_cseq = cseq;
			d->expires = d_act_time()+pcscf_dialogs_expiration_time;			
		}
	}
	
	d_unlock(d->hash);
	
	print_p_dialogs(L_INFO);
	
	return CSCF_RETURN_TRUE;	
}


/**
 * Drops and deletes a dialog.
 * @param msg - the request/response
 * @param str1 - direction - "orig" or "term"
 * @param str2 - not used
 * @returns #CSCF_RETURN_TRUE if ok, #CSCF_RETURN_FALSE if not or #CSCF_RETURN_BREAK on error 
 */
int P_drop_dialog(struct sip_msg* msg, char* str1, char* str2)
{
	str call_id;
	p_dialog *d;
	int hash;
	str host;
	int port,transport;
	struct sip_msg *req;
	
	
	if (!find_dialog_contact(msg,str1,&host,&port,&transport)){
		LOG(L_ERR,"ERR:"M_NAME":P_is_in_dialog(): Error retrieving %s contact\n",str1);
		return CSCF_RETURN_BREAK;
	}		
		
	call_id = cscf_get_call_id(msg,0);
	if (!call_id.len)
		return CSCF_RETURN_FALSE;

	LOG(L_INFO,"DBG:"M_NAME":P_drop_dialog(%s): Call-ID <%.*s> %d://%.*s:%d\n",
		str1,call_id.len,call_id.s,
		transport,host.len,host.s,port);

	d = get_p_dialog(call_id,host,port,transport);
	if (!d && msg->first_line.type==SIP_REPLY){
		/* Try to get the dialog from the request */
		req = cscf_get_request_from_reply(msg);		
		if (!find_dialog_contact(req,str1,&host,&port,&transport)){
			LOG(L_ERR,"ERR:"M_NAME":P_update_dialog(%s): Error retrieving %s contact\n",str1,str1);
			return CSCF_RETURN_BREAK;
		}		
		d = get_p_dialog(call_id,host,port,transport);		
	}
	if (!d){
		LOG(L_INFO,"INFO:"M_NAME":P_drop_dialog: dialog does not exists!\n");	
		return CSCF_RETURN_FALSE;
	}

	hash = d->hash;
	
	del_p_dialog(d);
		
	d_unlock(hash);
	
	print_p_dialogs(L_INFO);
	
	return CSCF_RETURN_TRUE;	
}

/**
 * Drop all dialogs belonging to one contact.
 *  on deregistration for example.
 * @param host - host that originates/terminates this dialog
 * @param port - port that originates/terminates this dialog
 * @param transport - transport that originates/terminates this dialog
 * @returns the number of dialogs dropped 
 */
int P_drop_all_dialogs(str host,int port, int transport)
{
	p_dialog *d,*dn;
	int i,cnt=0;;
	
	LOG(L_DBG,"DBG:"M_NAME":P_drop_all_dialogs: Called for <%d://%.*s:%d>\n",transport,host.len,host.s,port);

	for(i=0;i<p_dialogs_hash_size;i++){
		d_lock(i);
			d = p_dialogs[i].head;
			while(d){
				dn = d->next;
				if (d->transport == transport &&
					d->port == port &&
					d->host.len == host.len &&
					strncasecmp(d->host.s,host.s,host.len)==0) {
					del_p_dialog(d);
					cnt++;
				}						
				d = dn;
			}
		d_unlock(i);
	}
//	print_p_dialogs(L_INFO);	
	return cnt;
}

/**
 * Checks if the message follows the saved dialog routes.
 * @param msg - the SIP request
 * @param str1 - direction - "orig" or "term"
 * @param str2 - not used
 * @returns #CSCF_RETURN_TRUE if ok, #CSCF_RETURN_FALSE if not or #CSCF_RETURN_BREAK on error 
 */
int P_follows_dialog_routes(struct sip_msg *msg,char *str1,char *str2)
{
	int i;
	struct hdr_field *hdr=0;
	rr_t *r;
	p_dialog *d;
	str call_id,host;
	int port,transport;
	
	if (!find_dialog_contact(msg,str1,&host,&port,&transport)){
		LOG(L_ERR,"ERR:"M_NAME":P_follows_dialog_routes(): Error retrieving %s contact\n",str1);
		return CSCF_RETURN_BREAK;
	}		
		
	call_id = cscf_get_call_id(msg,0);
	if (!call_id.len)
		return CSCF_RETURN_FALSE;

	LOG(L_DBG,"DBG:"M_NAME":P_follows_dialog_routes(%s): Call-ID <%.*s> %d://%.*s:%d\n",
		str1,call_id.len,call_id.s,
		transport,host.len,host.s,port);

	d = get_p_dialog(call_id,host,port,transport);
	if (!d){
		LOG(L_ERR,"ERR:"M_NAME":P_follows_dialog_routes: dialog does not exists!\n");	
		return CSCF_RETURN_FALSE;
	}
	/* todo - fix this to match exactly the first request */
	if (d->first_cseq == cscf_get_cseq(msg,0) &&
		d->method == get_dialog_method(msg->first_line.u.request.method)){
		LOG(L_INFO,"INF:"M_NAME":P_follows_dialog_routes: this looks like the initial request (retransmission?)!\n");
		goto ok;		
	}

	hdr = cscf_get_next_route(msg,0);
	r = 0;
	if (!hdr){
		if (d->routes_cnt==0) goto ok;
		else goto nok;
	}
	r = (rr_t*) hdr->parsed;	
	for(i=0;i<d->routes_cnt;i++){
		LOG(L_DBG,"DBG:"M_NAME":P_follows_dialog_routes:  must <%.*s>\n",
			d->routes[i].len,d->routes[i].s);		
		if (!r) {
			hdr = cscf_get_next_route(msg,hdr);
			if (!hdr) goto nok;
			r = (rr_t*) hdr->parsed;	
		}
		LOG(L_DBG,"DBG:"M_NAME":P_follows_dialog_routes: src %.*s\n",
			r->nameaddr.uri.len,r->nameaddr.uri.s);		
		if (r->nameaddr.uri.len==d->routes[i].len &&
				strncasecmp(r->nameaddr.uri.s,
					d->routes[i].s,d->routes[i].len)==0)
		{
			LOG(L_DBG,"DBG:"M_NAME":P_follows_dialog_routes: src match\n");		
		} else {
			LOG(L_DBG,"DBG:"M_NAME":P_follows_dialog_routes: found <%.*s>\n",
				r->nameaddr.uri.len,r->nameaddr.uri.s);					
			goto nok;
		}
		r = r->next;
	}
	if (r) {
		LOG(L_DBG,"DBG:"M_NAME":P_follows_dialog_routes: still has some extra Routes\n");		
		goto nok;
	}
	else 
		if (cscf_get_next_route(msg,hdr)) goto nok;
	
ok:
	if (d) d_unlock(d->hash);
	return CSCF_RETURN_TRUE;	
nok:
	if (d) d_unlock(d->hash);
	return CSCF_RETURN_FALSE;		
}

static str route_s={"Route: <",8};
static str route_1={">, <",4};
static str route_e={">\r\n",3};
/**
 * Inserts the Route header containing the dialog routes to be enforced
 * @param msg - the SIP message to add to
 * @param str1 - the value to insert - !!! quoted if needed
 * @param str2 - not used
 * @returns #CSCF_RETURN_TRUE if ok, #CSCF_RETURN_FALSE if not or #CSCF_RETURN_BREAK on error 
 */
int P_enforce_dialog_routes(struct sip_msg *msg,char *str1,char*str2)
{
	int i;
	str newuri={0,0};
	p_dialog *d;
	str call_id,host;
	int port,transport;
	str x;
		
	if (!find_dialog_contact(msg,str1,&host,&port,&transport)){
		LOG(L_ERR,"ERR:"M_NAME":P_enforce_dialog_routes(): Error retrieving %s contact\n",str1);
		return CSCF_RETURN_BREAK;
	}		
		
	call_id = cscf_get_call_id(msg,0);
	if (!call_id.len)
		return CSCF_RETURN_FALSE;

	LOG(L_INFO,"DBG:"M_NAME":P_enforce_dialog_routes(%s): Call-ID <%.*s> %d://%.*s:%d\n",
		str1,call_id.len,call_id.s,
		transport,host.len,host.s,port);

	d = get_p_dialog(call_id,host,port,transport);
	if (!d){
		LOG(L_ERR,"ERR:"M_NAME":P_enforce_dialog_routes: dialog does not exists!\n");	
		return CSCF_RETURN_FALSE;
	}

	if (!d->routes_cnt){
		d_unlock(d->hash);
		return CSCF_RETURN_TRUE;
	}

	x.len = route_s.len + route_e.len + (d->routes_cnt-1)*route_1.len;
	for(i=0;i<d->routes_cnt;i++)
		x.len+=d->routes[i].len;
			
	x.s = pkg_malloc(x.len);
	if (!x.s){
		LOG(L_ERR, "ERR:"M_NAME":P_enforce_dialog_routes: Error allocating %d bytes\n",
			x.len);
		x.len=0;
		d_unlock(d->hash);
		return CSCF_RETURN_ERROR;
	}
	x.len=0;
	STR_APPEND(x,route_s);
	for(i=0;i<d->routes_cnt;i++){
		if (i) STR_APPEND(x,route_1);
		STR_APPEND(x,d->routes[i]);
	}	
	STR_APPEND(x,route_e);
	
	newuri.s = pkg_malloc(d->routes[0].len);
	if (!newuri.s){
		LOG(L_ERR, "ERR:"M_NAME":P_enforce_dialog_routes: Error allocating %d bytes\n",
			d->routes[0].len);
		d_unlock(d->hash);
		return CSCF_RETURN_ERROR;
	}
	newuri.len = d->routes[0].len;
	memcpy(newuri.s,d->routes[0].s,newuri.len);
	if (msg->dst_uri.s) pkg_free(msg->dst_uri.s);
	msg->dst_uri = newuri;
	
	//LOG(L_ERR,"%.*s",x.len,x.s);
	d_unlock(d->hash);
	if (cscf_add_header_first(msg,&x,HDR_ROUTE_T)) {
		if (cscf_del_all_headers(msg,HDR_ROUTE_T))
			return CSCF_RETURN_TRUE;
		else {
			LOG(L_ERR,"ERR:"M_NAME":P_enforce_dialog_routes: new Route headers added, but failed to drop old ones.\n");
			return CSCF_RETURN_ERROR;
		}
	}
	else {
		if (x.s) pkg_free(x.s);
		return CSCF_RETURN_ERROR;
	}
}




/**
 * Returns the p_dialog_direction from the direction string.
 * @param direction - "orig" or "term"
 * @returns the p_dialog_direction if ok or #DLG_MOBILE_UNKNOWN if not found
 */
static inline enum p_dialog_direction get_dialog_direction(char *direction)
{
	switch(direction[0]){
		case 'o':
		case 'O':
		case '0':
			return DLG_MOBILE_ORIGINATING;
		case 't':
		case 'T':
		case '1':
			return DLG_MOBILE_TERMINATING;
		default:
			LOG(L_CRIT,"ERR:"M_NAME":get_dialog_direction(): Unknown direction %s",direction);
			return DLG_MOBILE_UNKNOWN;
	}
}

static str s_record_route_s={"Record-Route: <",15};
static str s_record_route_e={";lr>\r\n",6};
/**
 * Record routes, with given user as parameter.
 * @param msg - the SIP message to add to
 * @param str1 - direction - "orig" or "term"
 * @param str2 - not used
 * @returns #CSCF_RETURN_TRUE if ok, #CSCF_RETURN_FALSE if not or #CSCF_RETURN_BREAK on error
 */ 
int P_record_route(struct sip_msg *msg,char *str1,char *str2)
{
	str rr;
	str u = {0,0},scheme={0,0},pcscf={0,0};
	
	enum p_dialog_direction dir = get_dialog_direction(str1);
	
	switch (dir){
		case DLG_MOBILE_ORIGINATING:
			STR_PKG_DUP(rr,pcscf_record_route_mo,"pkg");
			break;
		case DLG_MOBILE_TERMINATING:
			STR_PKG_DUP(rr,pcscf_record_route_mt,"pkg");
			break;
		default:
			u.s = str1;
			u.len = strlen(str1);
			if (pcscf_name_str.len>4 &&
				strncasecmp(pcscf_name_str.s,"sip:",4)==0){
				scheme.s = pcscf_name_str.s;
				scheme.len = 4;
			}else if (pcscf_name_str.len>5 &&
				strncasecmp(pcscf_name_str.s,"sips:",5)==0){
				scheme.s = pcscf_name_str.s;
				scheme.len = 4;
			}
			pcscf.s = scheme.s+scheme.len;
			pcscf.len = pcscf_name_str.len - scheme.len;
			
			rr.len = s_record_route_s.len+scheme.len+u.len+1+pcscf.len+s_record_route_e.len;
			rr.s = pkg_malloc(rr.len);
			if (!rr.s){
				LOG(L_ERR,"ERR:"M_NAME":P_record_route: error allocating %d bytes!\n",rr.len);	
				return CSCF_RETURN_BREAK;
			}
			rr.len = 0;
			STR_APPEND(rr,s_record_route_s);
			STR_APPEND(rr,scheme);
			STR_APPEND(rr,u);
			rr.s[rr.len++]='@';
			STR_APPEND(rr,pcscf);
			STR_APPEND(rr,s_record_route_e);					
	}
	
	if (cscf_add_header_first(msg,&rr,HDR_RECORDROUTE_T)) return CSCF_RETURN_TRUE;
	else{
		if (rr.s) pkg_free(rr.s);
		return CSCF_RETURN_BREAK;
	}
}

/**
 * The dialog timer looks for expired dialogs and removes them.
 * @param ticks - the current time
 * @param param - pointer to the dialogs list
 */
void dialog_timer(unsigned int ticks, void* param)
{
	p_dialog *d,*dn;
	int i;
	
	LOG(L_DBG,"DBG:"M_NAME":dialog_timer: Called at %d\n",ticks);
	if (!p_dialogs) p_dialogs = (p_dialog_hash_slot*)param;

	d_act_time();
	
	for(i=0;i<p_dialogs_hash_size;i++){
		d_lock(i);
			d = p_dialogs[i].head;
			while(d){
				dn = d->next;
				if (d->expires<=d_time_now) {
					del_p_dialog(d);
				}						
				d = dn;
			}
		d_unlock(i);
	}
	//print_p_dialogs(L_ERR);
}

