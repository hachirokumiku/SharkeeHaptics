using UnityEditor;
using UnityEngine;
using VRC.SDK3.Dynamics.Contact.Components;
using System.Linq;
using System.Collections.Generic;

// Defines the data structure for each contact bone mapping
[System.Serializable]
public class ContactBone
{
    // The base name (e.g., "Hand_L")
    public string Name;
    
    // The bone Transform set by the user/auto-find
    public Transform Bone;
    
    // The VRChat OSC Parameter Name for the RECEIVER (e.g., "Receiver_Hand_L")
    public string ReceiverParameterName;
    
    // The collision tag for the SENDER (e.g., "Hand")
    public string SenderCollisionTag;

    public ContactBone(string name, string senderTag)
    {
        Name = name;
        SenderCollisionTag = senderTag;
        ReceiverParameterName = $"Receiver_{name}";
        Bone = null;
    }
}

public class VRCContactSetupTool : EditorWindow
{
    private const float CONTACT_RADIUS = 0.05f;
    private const string WINDOW_TITLE = "VRC Contact Setup Tool";
    private const string MENU_PATH = "VRC_TOOLS/Contact Setup/Open Contact Setup Tool";

    private GameObject avatarRoot;
    
    // --- 59 VRChat Bone Definitions for Receivers & Senders ---
    // Tuple: (BaseName, SenderTag, HumanBodyBones unityBone)
    private static readonly (string name, string tag, HumanBodyBones unityBone)[] POSITION_DEFINITIONS_59 = new (string, string, HumanBodyBones)[]
    {
        // Core Body & Legs
        ("Head", "Head", HumanBodyBones.Head),
        ("Chest", "Chest", HumanBodyBones.Chest),
        ("Spine", "Spine", HumanBodyBones.Spine),
        ("Hips", "Hips", HumanBodyBones.Hips), 
        
        // Arms & Hands (L)
        ("UpperArm_L", "UpperArm_L", HumanBodyBones.LeftUpperArm),
        ("LowerArm_L", "LowerArm_L", HumanBodyBones.LeftLowerArm),
        ("Hand_L", "Hand", HumanBodyBones.LeftHand),
        
        // Arms & Hands (R)
        ("UpperArm_R", "UpperArm_R", HumanBodyBones.RightUpperArm),
        ("LowerArm_R", "LowerArm_R", HumanBodyBones.RightLowerArm),
        ("Hand_R", "Hand", HumanBodyBones.RightHand),
        
        // Legs & Feet (L)
        ("UpperLeg_L", "UpperLeg_L", HumanBodyBones.LeftUpperLeg),
        ("LowerLeg_L", "LowerLeg_L", HumanBodyBones.LeftLowerLeg),
        ("Foot_L", "Foot", HumanBodyBones.LeftFoot), 
        ("Toe_L", "Foot", HumanBodyBones.LeftToes),
        
        // Legs & Feet (R)
        ("UpperLeg_R", "UpperLeg_R", HumanBodyBones.RightUpperLeg),
        ("LowerLeg_R", "LowerLeg_R", HumanBodyBones.RightLowerLeg),
        ("Foot_R", "Foot", HumanBodyBones.RightFoot),
        ("Toe_R", "Foot", HumanBodyBones.RightToes),

        // Fingers (L)
        ("Thumb_L_A", "Thumb", HumanBodyBones.LeftThumbProximal),
        ("Thumb_L_B", "Thumb", HumanBodyBones.LeftThumbIntermediate),
        ("Thumb_L_C", "Thumb", HumanBodyBones.LeftThumbDistal),
        ("Index_L_A", "Index", HumanBodyBones.LeftIndexProximal),
        ("Index_L_B", "Index", HumanBodyBones.LeftIndexIntermediate),
        ("Index_L_C", "Index", HumanBodyBones.LeftIndexDistal),
        ("Middle_L_A", "Middle", HumanBodyBones.LeftMiddleProximal),
        ("Middle_L_B", "Middle", HumanBodyBones.LeftMiddleIntermediate),
        ("Middle_L_C", "Middle", HumanBodyBones.LeftMiddleDistal),
        ("Ring_L_A", "Ring", HumanBodyBones.LeftRingProximal),
        ("Ring_L_B", "Ring", HumanBodyBones.LeftRingIntermediate),
        ("Ring_L_C", "Ring", HumanBodyBones.LeftRingDistal),
        ("Little_L_A", "Little", HumanBodyBones.LeftLittleProximal),
        ("Little_L_B", "Little", HumanBodyBones.LeftLittleIntermediate),
        ("Little_L_C", "Little", HumanBodyBones.LeftLittleDistal),

        // Fingers (R)
        ("Thumb_R_A", "Thumb", HumanBodyBones.RightThumbProximal),
        ("Thumb_R_B", "Thumb", HumanBodyBones.RightThumbIntermediate),
        ("Thumb_R_C", "Thumb", HumanBodyBones.RightThumbDistal),
        ("Index_R_A", "Index", HumanBodyBones.RightIndexProximal),
        ("Index_R_B", "Index", HumanBodyBones.RightIndexIntermediate),
        ("Index_R_C", "Index", HumanBodyBones.RightIndexDistal),
        ("Middle_R_A", "Middle", HumanBodyBones.RightMiddleProximal),
        ("Middle_R_B", "Middle", HumanBodyBones.RightMiddleIntermediate),
        ("Middle_R_C", "Middle", HumanBodyBones.RightMiddleDistal),
        ("Ring_R_A", "Ring", HumanBodyBones.RightRingProximal),
        ("Ring_R_B", "Ring", HumanBodyBones.RightRingIntermediate),
        ("Ring_R_C", "Ring", HumanBodyBones.RightRingDistal),
        ("Little_R_A", "Little", HumanBodyBones.RightLittleProximal),
        ("Little_R_B", "Little", HumanBodyBones.RightLittleIntermediate),
        ("Little_R_C", "Little", HumanBodyBones.RightLittleDistal),

        // Shoulders & Neck
        ("Shoulder_L", "Shoulder", HumanBodyBones.LeftShoulder),
        ("Shoulder_R", "Shoulder", HumanBodyBones.RightShoulder),
        ("Neck", "Neck", HumanBodyBones.Neck),
    };
    
    // --- COMPREHENSIVE LIST: Collision Tags for Both Receivers (Listening) & Senders (Sending) ---
    private static readonly List<string> COLLISION_TAGS_ALL = new List<string>
    {
        // General Body
        "Hand",          
        "Foot",          
        "Head",          
        "Chest",          
        "Hips",
        "Hip",
        "Spine",
        "Neck",
        "Shoulder",
        "Elbow",
        "Knee",

        // Arm and Leg Segments
        "UpperArm",
        "LowerArm",
        "UpperLeg",
        "LowerLeg",

        // Fingers (The base names used by the senders)
        "Thumb",
        "Index",          
        "Middle",
        "Ring",
        "Little",
        
        // Catch-all/Custom Tags
        "Finger",        
        "CustomTag"      
    };

    // Runtime array to hold the user's bone selections
    private ContactBone[] contactBones;
    
    // Used for scrolling the long list of bone fields
    private Vector2 scrollPosition;

    [MenuItem(MENU_PATH, false, 1)]
    private static void Init()
    {
        VRCContactSetupTool window = (VRCContactSetupTool)EditorWindow.GetWindow(typeof(VRCContactSetupTool), false, WINDOW_TITLE);
        window.Show();
    }
    
    private void OnEnable()
    {
        InitializeBones();
        
        // Initial attempt to pre-populate the avatar root from selection
        if (Selection.activeGameObject != null && Selection.activeGameObject.GetComponentInParent<Animator>() != null)
        {
            avatarRoot = Selection.activeGameObject.transform.root.gameObject;
            if (avatarRoot != null)
            {
                AutoFindBones(false); // Do not show success dialog on initial load
            }
        }
    }
    
    private void InitializeBones()
    {
        if (contactBones == null || contactBones.Length != POSITION_DEFINITIONS_59.Length)
        {
            contactBones = new ContactBone[POSITION_DEFINITIONS_59.Length];
            for (int i = 0; i < POSITION_DEFINITIONS_59.Length; i++)
            {
                // Initialize ContactBone with BaseName and SenderTag
                contactBones[i] = new ContactBone(POSITION_DEFINITIONS_59[i].name, POSITION_DEFINITIONS_59[i].tag);
            }
        }
    }

    // 🔨 --- Core Auto-Find Logic ---
    private void AutoFindBones(bool showDialog = true)
    {
        if (avatarRoot == null)
        {
            if (showDialog) EditorUtility.DisplayDialog("Error", "Please set the Avatar Root first to enable auto-finding.", "OK");
            return;
        }

        Animator animator = avatarRoot.GetComponentInChildren<Animator>();
        if (animator == null || !animator.isHuman)
        {
            if (showDialog) EditorUtility.DisplayDialog("Error", "Could not find a valid Humanoid Animator component on the Avatar Root.", "OK");
            return;
        }

        int bonesFound = 0;
        for (int i = 0; i < POSITION_DEFINITIONS_59.Length; i++)
        {
            // Use the Unity HumanBodyBones enumeration to find the bone Transform
            Transform bone = animator.GetBoneTransform(POSITION_DEFINITIONS_59[i].unityBone);
            if (bone != null)
            {
                contactBones[i].Bone = bone;
                bonesFound++;
            }
        }

        Repaint();
        
        if (showDialog)
        {
            EditorUtility.DisplayDialog("Auto-Find Complete", 
                                        $"Found and mapped {bonesFound}/{POSITION_DEFINITIONS_59.Length} bones on {avatarRoot.name}.", 
                                        "OK");
        }
        else
        {
            Debug.Log($"Auto-Find completed for {avatarRoot.name}. Found and mapped {bonesFound}/{POSITION_DEFINITIONS_59.Length} bones.");
        }
    }

    // 🧹 --- Cleanup/Removal Logic ---

    private void CleanupAllContacts()
    {
        InitializeBones();
        if (contactBones == null || contactBones.Length == 0) return;
        
        if (avatarRoot == null)
        {
             EditorUtility.DisplayDialog("Error", "Please set the Avatar Root first.", "OK");
             return;
        }

        if (!EditorUtility.DisplayDialog("Confirm Cleanup", 
            "Are you sure you want to REMOVE ALL VRC Contact Receivers and Senders from the mapped bones and their child objects?", 
            "Yes, Remove All", "Cancel"))
        {
            return;
        }

        int removedCount = 0;
        
        Undo.IncrementCurrentGroup();
        int mainUndoGroup = Undo.GetCurrentGroup();

        EditorUtility.DisplayProgressBar("Cleaning Up Contacts", "Starting component removal...", 0f);

        try
        {
            var mappedBones = contactBones.Where(b => b.Bone != null).ToList();
            int totalBones = mappedBones.Count;
            
            for (int i = 0; i < totalBones; i++)
            {
                Transform boneTransform = mappedBones[i].Bone;
                string progressTitle = $"Cleaning up {boneTransform.name}... ({i + 1}/{totalBones})";
                EditorUtility.DisplayProgressBar("Cleaning Up Contacts", progressTitle, (float)i / totalBones);

                // Find all Contact components in children (including on the bone itself)
                VRCContactReceiver[] receivers = boneTransform.GetComponentsInChildren<VRCContactReceiver>();
                VRCContactSender[] senders = boneTransform.GetComponentsInChildren<VRCContactSender>();

                // Remove Receivers
                foreach (var receiver in receivers)
                {
                    // Check if the component is parented under one of our mapped bones
                    if (receiver.transform.parent != null && mappedBones.Any(b => b.Bone == receiver.transform.parent))
                    {
                        Undo.DestroyObjectImmediate(receiver.gameObject);
                        removedCount++;
                    }
                    else if (receiver.transform == boneTransform)
                    {
                         Undo.DestroyObjectImmediate(receiver.gameObject);
                         removedCount++;
                    }
                }
                
                // Remove Senders
                foreach (var sender in senders)
                {
                    // Check if the component is parented under one of our mapped bones
                    if (sender.transform.parent != null && mappedBones.Any(b => b.Bone == sender.transform.parent))
                    {
                        Undo.DestroyObjectImmediate(sender.gameObject);
                        removedCount++;
                    }
                    else if (sender.transform == boneTransform)
                    {
                         Undo.DestroyObjectImmediate(sender.gameObject);
                         removedCount++;
                    }
                }
            }
        }
        finally
        {
            EditorUtility.ClearProgressBar();
        }
        
        Undo.SetCurrentGroupName("Cleanup All VRC Contacts");
        Undo.CollapseUndoOperations(mainUndoGroup);

        EditorUtility.DisplayDialog("Cleanup Complete", 
                                    $"Successfully removed {removedCount} contact components (Senders & Receivers) from the mapped bones' hierarchy.", 
                                    "Done!");
        Repaint();
    }


    // ➕ --- Batch Placement Logic (Receivers) ---
    
    private void PlaceAllReceivers()
    {
        InitializeBones();
        if (contactBones == null || contactBones.Length == 0) return;

        // Filter for bones that are mapped AND do NOT already have a matching receiver.
        var bonesToPlace = contactBones.Where(b => b.Bone != null && !_ReceiverExists(b.Bone, b.ReceiverParameterName))
                                       .ToList();

        int totalMapped = contactBones.Count(b => b.Bone != null);
        int totalToPlace = bonesToPlace.Count;
        int skippedCount = totalMapped - totalToPlace;

        if (totalToPlace == 0)
        {
            EditorUtility.DisplayDialog("Warning", $"No new Receivers needed on mapped bones. Skipped {skippedCount} existing receivers.", "OK");
            return;
        }

        int placedCount = 0;
        // Start a single, unified undo group for the batch operation
        Undo.IncrementCurrentGroup();
        int mainUndoGroup = Undo.GetCurrentGroup();

        EditorUtility.DisplayProgressBar("Placing VRC Contact Receivers", "Starting placement...", 0f);

        try
        {
            for (int i = 0; i < bonesToPlace.Count; i++)
            {
                ContactBone boneMapping = bonesToPlace[i];
                string progressTitle = $"Placing receiver on {boneMapping.Name}... ({i + 1}/{totalToPlace})";
                EditorUtility.DisplayProgressBar("Placing VRC Contact Receivers", progressTitle, (float)i / totalToPlace);
                
                // Call the internal placement logic, suppressing individual dialogs
                _PlaceReceiverInternal(boneMapping, false);
                placedCount++;
            }
        }
        finally
        {
            EditorUtility.ClearProgressBar();
        }

        Undo.SetCurrentGroupName("Place All VRC Contact Receivers");
        Undo.CollapseUndoOperations(mainUndoGroup);

        EditorUtility.DisplayDialog("Receiver Placement Complete", 
                                    $"Successfully placed {placedCount} new receivers on {totalToPlace} bones.\nSkipped {skippedCount} existing or duplicate receivers.", 
                                    "Done!");
        Repaint(); 
    }
    
    // Checks if a receiver with the exact parameter name already exists on or under the bone transform
    private bool _ReceiverExists(Transform bone, string parameterName)
    {
        return bone.GetComponentsInChildren<VRCContactReceiver>()
                   .Any(r => r.parameter == parameterName);
    }

    // ➕ --- Batch Placement Logic (Senders) ---

    private void PlaceAllSenders()
    {
        InitializeBones();
        if (contactBones == null || contactBones.Length == 0) return;

        // Filter for bones that are mapped AND do NOT already have a matching sender (multi-tag).
        var bonesToPlace = contactBones.Where(b => b.Bone != null && !_SenderExists(b.Bone))
                                       .ToList();

        int totalMapped = contactBones.Count(b => b.Bone != null);
        int totalToPlace = bonesToPlace.Count;
        int skippedCount = totalMapped - totalToPlace;

        if (totalToPlace == 0)
        {
            EditorUtility.DisplayDialog("Warning", $"No new Senders needed on mapped bones. Skipped {skippedCount} existing senders.", "OK");
            return;
        }

        int placedCount = 0;
        // Start a single, unified undo group for the batch operation
        Undo.IncrementCurrentGroup();
        int mainUndoGroup = Undo.GetCurrentGroup();

        EditorUtility.DisplayProgressBar("Placing VRC Contact Senders", "Starting placement...", 0f);

        try
        {
            for (int i = 0; i < bonesToPlace.Count; i++)
            {
                ContactBone boneMapping = bonesToPlace[i];
                string progressTitle = $"Placing sender on {boneMapping.Name}... ({i + 1}/{totalToPlace})";
                EditorUtility.DisplayProgressBar("Placing VRC Contact Senders", progressTitle, (float)i / totalToPlace);
                
                // Call the internal placement logic, suppressing individual dialogs
                _PlaceSenderInternal(boneMapping, false);
                placedCount++;
            }
        }
        finally
        {
            EditorUtility.ClearProgressBar();
        }

        Undo.SetCurrentGroupName("Place All VRC Contact Senders");
        Undo.CollapseUndoOperations(mainUndoGroup);

        EditorUtility.DisplayDialog("Sender Placement Complete", 
                                    $"Successfully placed {placedCount} new senders on {totalToPlace} bones.\nSkipped {skippedCount} existing or duplicate senders.", 
                                    "Done!");
        Repaint(); 
    }
    
    // Checks if a sender with the required multi-tag configuration already exists on or under the bone transform
    private bool _SenderExists(Transform bone)
    {
        // Compare the lists to see if all tags are present and the count matches
        var requiredTags = COLLISION_TAGS_ALL;
        
        return bone.GetComponentsInChildren<VRCContactSender>()
                   .Any(s => s.radius == CONTACT_RADIUS && 
                             s.collisionTags != null &&
                             s.collisionTags.Count == requiredTags.Count && 
                             s.collisionTags.All(requiredTags.Contains));
    }


    // 🎨 --- Editor Window GUI ---

    private void OnGUI()
    {
        InitializeBones();

        GUILayout.Label("🛠️ Avatar Configuration", EditorStyles.boldLabel);
        
        GameObject oldAvatarRoot = avatarRoot;

        // Draw the Object field, potentially updating avatarRoot
        avatarRoot = (GameObject)EditorGUILayout.ObjectField(
            "Avatar Root (Required for Auto-Find):", 
            avatarRoot, 
            typeof(GameObject), 
            true
        );

        if (avatarRoot != oldAvatarRoot && avatarRoot != null)
        {
            // Clear current bone mappings and auto-find
            foreach (var bone in contactBones)
            {
                bone.Bone = null;
            }
            AutoFindBones(false);
        }

        // --- Auto-Find & Cleanup Button ---
        GUI.enabled = avatarRoot != null;
        EditorGUILayout.BeginHorizontal();
        if (GUILayout.Button("Manually Re-run Auto-Find All 59 Bones", GUILayout.Height(25)))
        {
            AutoFindBones(true);
        }
        if (GUILayout.Button("🧹 Cleanup ALL Senders/Receivers", GUILayout.Height(25), GUILayout.Width(200)))
        {
            CleanupAllContacts();
        }
        EditorGUILayout.EndHorizontal();
        
        // --- Batch Placement Buttons ---
        bool anyBonesMapped = contactBones != null && contactBones.Any(b => b.Bone != null);
        GUI.enabled = avatarRoot != null && anyBonesMapped;

        EditorGUILayout.Space(5);
        EditorGUILayout.BeginHorizontal();
        if (GUILayout.Button("🚀 Place ALL 59 RECEIVERS (OSC)", GUILayout.Height(35)))
        {
            PlaceAllReceivers();
        }
        if (GUILayout.Button("💥 Place ALL 59 SENDERS", GUILayout.Height(35)))
        {
            PlaceAllSenders();
        }
        EditorGUILayout.EndHorizontal();
        GUI.enabled = true; 

        EditorGUILayout.Space(15);
        
        if (avatarRoot == null)
        {
            EditorGUILayout.HelpBox("Select your **Avatar Root** above to automatically find and map all 59 VRChat humanoid bones.", MessageType.Info);
        }

        GUILayout.Label($"Bone Mapping Status: ({contactBones.Count(b => b.Bone != null)}/{POSITION_DEFINITIONS_59.Length} Mapped)", EditorStyles.boldLabel);
        
        scrollPosition = EditorGUILayout.BeginScrollView(scrollPosition);

        bool allMapped = true;
        
        // Create an Object Field and two buttons for each contact position
        for (int i = 0; i < contactBones.Length; i++)
        {
            ContactBone boneMapping = contactBones[i];
            
            EditorGUILayout.BeginHorizontal();
            
            // 1. Bone Selection Field
            boneMapping.Bone = (Transform)EditorGUILayout.ObjectField(
                $"{boneMapping.Name}:", 
                boneMapping.Bone, 
                typeof(Transform), 
                true
            );
            
            contactBones[i] = boneMapping;

            // 2. Receiver Placement Button
            GUI.enabled = boneMapping.Bone != null;
            if (GUILayout.Button("Rec. (OSC)", GUILayout.Width(75)))
            {
                _PlaceReceiverInternal(boneMapping, true);
            }
            
            // 3. Sender Placement Button
            if (GUILayout.Button("Send (Tag)", GUILayout.Width(75)))
            {
                _PlaceSenderInternal(boneMapping, true);
            }
            GUI.enabled = true;

            EditorGUILayout.EndHorizontal();
            
            if (boneMapping.Bone == null)
            {
                allMapped = false;
            }
        }
        
        EditorGUILayout.EndScrollView();

        EditorGUILayout.Space(15);
        
        // General instructions and status
        if (allMapped)
        {
            EditorGUILayout.HelpBox("All 59 VRChat positions are mapped! Click the batch buttons above, or individual buttons below, to add the contact components.", MessageType.Info);
        }
        else if (avatarRoot != null)
        {
            EditorGUILayout.HelpBox("Some bones are missing. Drag the corresponding bone from the Hierarchy into the empty slot, then click a batch button or individual 'Rec.' / 'Send' buttons.", MessageType.Warning);
        }
    }

    // --- Core Placement Logic - RECEIVER ---

    /// <summary>
    /// Internal logic for creating and configuring the VRCContactReceiver.
    /// Includes a check for duplicates.
    /// </summary>
    private void _PlaceReceiverInternal(ContactBone boneMapping, bool showDialog)
    {
        Transform boneTransform = boneMapping.Bone;
        string conciseName = boneMapping.Name;
        string parameterName = boneMapping.ReceiverParameterName;
        
        if (boneTransform == null)
        {
            if (showDialog) EditorUtility.DisplayDialog("Error", $"The bone Transform is not set for '{conciseName}'.", "OK");
            return;
        }
        
        // Check for duplicates
        if (_ReceiverExists(boneTransform, parameterName))
        {
            if (showDialog) EditorUtility.DisplayDialog("Warning", $"A VRCContactReceiver with parameter '{parameterName}' already exists on or under {boneTransform.name}. Skipping.", "OK");
            return;
        }
        
        string contactObjectName = $"Receiver_{conciseName}"; 
        
        int undoGroup = 0;
        if (showDialog) 
        {
             Undo.IncrementCurrentGroup();
             Undo.SetCurrentGroupName($"Place VRC Contact Receiver: {conciseName}");
             undoGroup = Undo.GetCurrentGroup();
        }

        // 1. Create the new child GameObject
        GameObject receiverGO = new GameObject(contactObjectName);
        Undo.RegisterCreatedObjectUndo(receiverGO, $"Create Receiver GO {contactObjectName}");
        
        // Set it as a child of the target bone and zero out its position
        receiverGO.transform.SetParent(boneTransform);
        receiverGO.transform.localPosition = Vector3.zero;
        receiverGO.transform.localRotation = Quaternion.identity;
        receiverGO.transform.localScale = Vector3.one; 

        // 2. Add the VRCContactReceiver component and register the action for Undo
        VRCContactReceiver receiver = Undo.AddComponent<VRCContactReceiver>(receiverGO);
        
        // 3. Configure the component
        receiver.receiverType = VRCContactReceiver.ReceiverType.Proximity; 
        receiver.radius = CONTACT_RADIUS;
        
        // CRITICAL: Explicitly set the VRChat Parameter Name for OSC
        receiver.parameter = parameterName;
        
        // Set the collision tags - NOW LISTENS TO ALL STANDARD SENDERS
        receiver.collisionTags = COLLISION_TAGS_ALL;
        
        Debug.Log($"Successfully added VRCContactReceiver for '{conciseName}'. Parameter: '{parameterName}'. Now listens to {COLLISION_TAGS_ALL.Count} tags.");
        
        if (showDialog) 
        {
             Undo.CollapseUndoOperations(undoGroup);
             EditorUtility.DisplayDialog("Receiver Success!", 
                                         $"VRC Contact Receiver for '{conciseName}' successfully added to {boneTransform.name}. It is configured to listen to ALL standard VRChat sender tags.", 
                                         "Done!");
        }
    }
    
    // --- Core Placement Logic - SENDER (Contact Point) ---

    /// <summary>
    /// Internal logic for creating and configuring the VRCContactSender.
    /// Includes a check for duplicates.
    /// </summary>
    private void _PlaceSenderInternal(ContactBone boneMapping, bool showDialog)
    {
        Transform boneTransform = boneMapping.Bone;
        string conciseName = boneMapping.Name;
        
        if (boneTransform == null)
        {
            if (showDialog) EditorUtility.DisplayDialog("Error", $"The bone Transform is not set for '{conciseName}'.", "OK");
            return;
        }
        
        // Check for duplicates
        if (_SenderExists(boneTransform))
        {
            if (showDialog) EditorUtility.DisplayDialog("Warning", $"A VRCContactSender with multi-tag configuration and radius {CONTACT_RADIUS} already exists on or under {boneTransform.name}. Skipping.", "OK");
            return;
        }
        
        string contactObjectName = $"Sender_{conciseName}"; 
        
        int undoGroup = 0;
        if (showDialog) 
        {
             Undo.IncrementCurrentGroup();
             Undo.SetCurrentGroupName($"Place VRC Contact Sender: {conciseName}");
             undoGroup = Undo.GetCurrentGroup();
        }

        // 1. Create the new child GameObject
        GameObject senderGO = new GameObject(contactObjectName);
        Undo.RegisterCreatedObjectUndo(senderGO, $"Create Sender GO {contactObjectName}");
        
        // Set it as a child of the target bone and zero out its position
        senderGO.transform.SetParent(boneTransform);
        senderGO.transform.localPosition = Vector3.zero;
        senderGO.transform.localRotation = Quaternion.identity;
        senderGO.transform.localScale = Vector3.one; 

        // 2. Add the VRCContactSender component and register the action for Undo
        VRCContactSender sender = Undo.AddComponent<VRCContactSender>(senderGO);
        
        // 3. Configure the component
        sender.radius = CONTACT_RADIUS;
        
        // Set ALL standard tags on the Sender
        sender.collisionTags = COLLISION_TAGS_ALL;
        
        Debug.Log($"Successfully added VRCContactSender for '{conciseName}'. Tags: All {COLLISION_TAGS_ALL.Count} standard tags.");
        
        if (showDialog) 
        {
             Undo.CollapseUndoOperations(undoGroup);
             EditorUtility.DisplayDialog("Sender Success!", 
                                         $"VRC Contact Sender for '{conciseName}' successfully added to {boneTransform.name}. It is configured to send using ALL {COLLISION_TAGS_ALL.Count} standard VRChat receiver tags.", 
                                         "Done!");
        }
    }
}
