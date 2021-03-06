// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_DRIVE_CHANGE_LIST_LOADER_H_
#define CHROME_BROWSER_CHROMEOS_DRIVE_CHANGE_LIST_LOADER_H_

#include <string>

#include "base/callback_forward.h"
#include "base/memory/scoped_vector.h"
#include "base/observer_list.h"
#include "chrome/browser/chromeos/drive/drive_resource_metadata.h"
#include "chrome/browser/google_apis/gdata_errorcode.h"
#include "googleurl/src/gurl.h"

namespace base {
class Value;
}

namespace google_apis {
class AboutResource;
class AppList;
class ResourceList;
}

namespace drive {

class DriveCache;
class ChangeListLoaderObserver;
class ChangeListProcessor;
class DriveScheduler;
class DriveWebAppsRegistry;

// Callback run as a response to SearchFromServer and LoadDirectoryFromServer.
typedef base::Callback<
    void(const ScopedVector<google_apis::ResourceList>& feed_list,
         DriveFileError error)> LoadFeedListCallback;

// Holds information needed to fetch contents of a directory.
// This object is copyable.
class DirectoryFetchInfo {
 public:
  DirectoryFetchInfo() : changestamp_(0) {}
  DirectoryFetchInfo(const std::string& resource_id,
                     int64 changestamp)
      : resource_id_(resource_id),
        changestamp_(changestamp) {
  }

  // Returns true if the object is empty.
  bool empty() const { return resource_id_.empty(); }

  // Resource ID of the directory.
  const std::string& resource_id() const { return resource_id_; }

  // Changestamp of the directory. The changestamp is used to determine if
  // the directory contents should be fetched.
  int64 changestamp() const { return changestamp_; }

 private:
  const std::string resource_id_;
  const int64 changestamp_;
};

// ChangeListLoader is used to load feeds from WAPI (codename for
// Documents List API) and load the cached proto file.
class ChangeListLoader {
 public:
  ChangeListLoader(
      DriveResourceMetadata* resource_metadata,
      DriveScheduler* scheduler,
      DriveWebAppsRegistry* webapps_registry,
      DriveCache* cache,
      scoped_refptr<base::SequencedTaskRunner> blocking_task_runner);
  ~ChangeListLoader();

  // Adds and removes the observer.
  void AddObserver(ChangeListLoaderObserver* observer);
  void RemoveObserver(ChangeListLoaderObserver* observer);

  // Starts root feed load from the cache, and runs |callback| to tell the
  // result to the caller.
  // |callback| must not be null.
  void LoadFromCache(const FileOperationCallback& callback);

  // Starts retrieving feed for a directory specified by |directory_resource_id|
  // from the server. Upon completion, |callback| is invoked.
  // |callback| must not be null.
  void LoadDirectoryFromServer(const std::string& directory_resource_id,
                               const FileOperationCallback& callback);

  // Starts retrieving search results for |search_query| from the server.
  // If |next_feed| is set, this is the feed url that will be fetched.
  // If |next_feed| is an empty string, the default URL is used.
  // If |shared_with_me| is true, it searches for the files shared to the user,
  // otherwise searches for the files owned by the user.
  // Upon completion, |feed_load_callback| is invoked.
  // |feed_load_callback| must not be null.
  void SearchFromServer(const std::string& search_query,
                        bool shared_with_me,
                        const GURL& next_feed,
                        const LoadFeedListCallback& feed_load_callback);

  // Initiates the chnage list loading from the server if the local
  // changestamp is older than the server changestamp.
  //
  // If |directory_fetch_info| is not empty, the directory will be fetched
  // first so the UI can show the directory contents instantly before the
  // entire change list loading is complete.
  // TODO(satorux): Implement the "fast-fetch" behavior.
  //
  // |callback| must not be null.
  void ReloadFromServerIfNeeded(const DirectoryFetchInfo& directory_fetch_info,
                                const FileOperationCallback& callback);

  // Updates whole directory structure feeds collected in |feed_list|.
  // Record file statistics as UMA histograms.
  //
  // See comments at ChangeListProcessor::ApplyFeeds() for
  // |is_delta_feed| and |root_feed_changestamp|.
  // |update_finished_callback| must not be null.
  void UpdateFromFeed(const ScopedVector<google_apis::ResourceList>& feed_list,
                      bool is_delta_feed,
                      int64 root_feed_changestamp,
                      const base::Closure& update_finished_callback);

  // Indicates whether there is a feed refreshing server request is in flight.
  bool refreshing() const { return refreshing_; }

 private:
  struct GetResourceListUiState;
  struct LoadFeedParams;
  struct LoadRootFeedParams;
  struct UpdateMetadataParams;

  // Starts root feed load from the server, with details specified in |params|.
  void LoadFromServer(scoped_ptr<LoadFeedParams> params);

  // Part of LoadDirectoryFromServer(). Called after
  // LoadFromServer() is complete.
  void LoadDirectoryFromServerAfterLoad(
      const std::string& directory_resource_id,
      const FileOperationCallback& callback,
      const ScopedVector<google_apis::ResourceList>& resource_list,
      DriveFileError error);

  // Part of LoadDirectoryFromServer(). Called after
  // DriveResourceMetadata::RefreshDirectory() is complete.
  void LoadDirectoryFromServerAfterRefresh(
      const FileOperationCallback& callback,
      DriveFileError error,
      const base::FilePath& directory_path);

  // Callback for handling root directory refresh from the cache.
  void OnProtoLoaded(LoadRootFeedParams* params, DriveFileError error);

  // Continues handling root directory refresh after the resource metadata
  // is fully loaded.
  void ContinueWithInitializedResourceMetadata(LoadRootFeedParams* params,
                                               DriveFileError error);

  // Helper callback for handling results of metadata retrieval initiated
  // from ReloadFromServerIfNeeded(). This method calls
  // CompareChangestampsAndLoadIfNeeded() to make a decision about whether or
  // not to fetch the change list.
  void OnGetAboutResource(
      const DirectoryFetchInfo& directory_fetch_info,
      const FileOperationCallback& callback,
      google_apis::GDataErrorCode status,
      scoped_ptr<google_apis::AboutResource> about_resource);

  // Compares |remote_changestamp| and |local_changestamp| and triggers
  // LoadFromServer if necessary.
  void CompareChangestampsAndLoadIfNeeded(
      const DirectoryFetchInfo& directory_fetch_info,
      const FileOperationCallback& callback,
      int64 remote_changestamp,
      int64 local_changestamp);

  // Callback for handling response from |DriveAPIService::GetAppList|.
  // If the application list is successfully parsed, passes the list to
  // Drive webapps registry.
  void OnGetAppList(google_apis::GDataErrorCode status,
                    scoped_ptr<google_apis::AppList> app_list);

  // Callback for handling feed content fetching while searching for file info.
  // This callback is invoked after async feed fetch operation that was
  // invoked by StartDirectoryRefresh() completes. This callback will update
  // the content of the refreshed directory object and continue initially
  // started FindEntryByPath() request.
  void UpdateMetadataFromFeedAfterLoadFromServer(
      const UpdateMetadataParams& params,
      const ScopedVector<google_apis::ResourceList>& feed_list,
      DriveFileError error);

  // Callback for handling response from |GDataWapiService::GetResourceList|.
  // Invokes |callback| when done.
  // |callback| must not be null.
  void OnGetResourceList(scoped_ptr<LoadFeedParams> params,
                         base::TimeTicks start_time,
                         google_apis::GDataErrorCode status,
                         scoped_ptr<google_apis::ResourceList> data);

  // Save filesystem to disk.
  void SaveFileSystem();

  // Callback for handling UI updates caused by feed fetching.
  void OnNotifyResourceListFetched(
      base::WeakPtr<GetResourceListUiState> ui_state);

  // Callback for ChangeListProcessor::ApplyFeeds.
  void NotifyDirectoryChanged(bool should_notify,
                              const base::Closure& update_finished_callback);

  // Callback for UpdateFromFeed.
  void OnUpdateFromFeed(const FileOperationCallback& load_finished_callback);

  DriveResourceMetadata* resource_metadata_;  // Not owned.
  DriveScheduler* scheduler_;  // Not owned.
  DriveWebAppsRegistry* webapps_registry_;  // Not owned.
  DriveCache* cache_;  // Not owned.
  scoped_refptr<base::SequencedTaskRunner> blocking_task_runner_;
  ObserverList<ChangeListLoaderObserver> observers_;
  scoped_ptr<ChangeListProcessor> change_list_processor_;

  // Indicates whether there is a feed refreshing server request is in flight.
  bool refreshing_;

  // Note: This should remain the last member so it'll be destroyed and
  // invalidate its weak pointers before any other members are destroyed.
  base::WeakPtrFactory<ChangeListLoader> weak_ptr_factory_;
  DISALLOW_COPY_AND_ASSIGN(ChangeListLoader);
};

}  // namespace drive

#endif  // CHROME_BROWSER_CHROMEOS_DRIVE_CHANGE_LIST_LOADER_H_
